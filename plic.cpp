#include <cstdint>

#include "plic.h"
#include "constants.h"
#include "hartexc.h"

// context assignment:
// even contexts for M mode, odd contexts for S mode
// context 0 for hart 0 M, context 1 for hart 0 S, context 2 for hart 1 M etc

uint32_t* int_prio_regs;
uint32_t* int_pend_regs;
uint32_t* int_en_regs;
uint32_t* int_prio_thres;

bool* int_handling;

#define PLIC_CTX_COUNT (2 * MACH_HART_COUNT)

void plic_init() {
  int_prio_regs = new uint32_t[PLIC_SOURCE_COUNT];
  int_pend_regs = new uint32_t[(PLIC_SOURCE_COUNT + 31) / 32]; // default behaviour is to round to zero; this forces it to round up for positive numbers
  int_en_regs = new uint32_t[(PLIC_SOURCE_COUNT + 31) / 32 * PLIC_CTX_COUNT] {0xFFFFFFFF};
  int_prio_thres = new uint32_t[PLIC_CTX_COUNT];
  
  int_handling = new bool[PLIC_CTX_COUNT] {false};
}

void plic_send_int(uint16_t source) {
  uint16_t regnum = source / 32;
  uint16_t regbit = source % 32;
  
  if (int_pend_regs[regnum] & (1 << regbit)) { // previous interrupt is pending!
    return;
  }
  int_pend_regs[regnum] |= 1 << regbit;
  
  plic_try_int(source);
}

void plic_try_int(uint16_t source) {
  uint16_t regnum = source / 32;
  uint16_t regbit = source % 32;
  if (!(int_pend_regs[regnum] & (1 << regbit)) ) { // this interrupt is not pending!
    return;
  }

  uint32_t int_prio = int_prio_regs[source];
  for (uint16_t i = 0; i < PLIC_CTX_COUNT; i++) {
    if (!int_handling[i] && int_prio_thres[i] < int_prio && plic_find_en(i, source)) {
      plic_notify_ctx(i);
    }
  }
}

void plic_try_int_all() {
  for (uint16_t i = 0; i < PLIC_SOURCE_COUNT; i++) {
    plic_try_int(i);
  }
}

bool plic_find_en(uint16_t ctx, uint16_t source) {
  return int_en_regs[(0x20) * ctx + source / 32] & (1 << (source % 32));
}

void plic_notify_ctx(uint16_t ctx) {
  if (ctx & 0b1) {
    hartlist[(ctx & (~0b1)) / 2].mip |= 1 << 9;
    //hartlist[(ctx & (~0b1)) / 2].sip |= 1 << 9;
  } else {
    hartlist[(ctx & (~0b1)) / 2].mip |= 1 << 11;
  }
  //setup_pending_int(hartlist[(ctx & (~0b1)) / 2]);
}

void plic_notify_finish_ctx(uint16_t ctx) {
  if (ctx & 0b1) {
    hartlist[(ctx & (~0b1)) / 2].mip &= ~(1 << 9);
    //hartlist[(ctx & (~0b1)) / 2].sip &= ~(1 << 9);
  } else {
    hartlist[(ctx & (~0b1)) / 2].mip &= ~(1 << 11);
  }
}

void plic_chk() {
  plic_try_int_all();
}

uint32_t highest_source = 0;
#define OFFSET_TO_CTX(X) (X >> 12) - 0x200

void* plic_r (uint64_t offset, [[maybe_unused]]uint8_t len) {
  uint64_t roffset = offset / 4; // reduced offset
  if (/*0 <= offset &&*/ roffset < PLIC_SOURCE_COUNT) {
    return &int_prio_regs[roffset];
  }
  if ((0x1000 / 4) <= roffset && roffset < (0x1000 / 4 + (PLIC_SOURCE_COUNT + 31) / 32)) {
    return &int_pend_regs[roffset - 0x1000/4];
  }
  if ((0x2000 / 4) <= roffset && roffset < (0x2000u / 4 + PLIC_CTX_COUNT * (1024/8))) {
    return &int_en_regs[roffset - 0x2000/4];
  }
  if (offset >= 0x200000 && ((offset & 0xFFF) == 0)) {
    return &int_prio_thres[OFFSET_TO_CTX(offset)];
  }
  if (offset >= 0x200000 && ((offset & 0xFFF) == 4)) {
    // interrupt claim
    uint32_t highest_prio = 0;
    highest_source = 0;
    for (uint16_t i = 0; i < PLIC_SOURCE_COUNT; i++) {
      if (int_prio_regs[i] > highest_prio) {
        if (plic_find_en(OFFSET_TO_CTX(offset), i)) {
          highest_prio = int_prio_regs[i];
          highest_source = i;
        }
      }
    }
    if (highest_source) {
      int_handling[OFFSET_TO_CTX(offset)] = true;
      int_pend_regs[highest_source / 32] &= ~(1 << (highest_source % 32));
    }
    return &highest_source;
  }
  return &ZERO;
}
void plic_w (uint64_t offset, void* dataptr, [[maybe_unused]]uint8_t len) {
  uint64_t roffset = offset / 4; // reduced offset
  if (/*0 <= offset &&*/ roffset < PLIC_SOURCE_COUNT) {
    int_prio_regs[roffset] = *(uint32_t*)dataptr;
  }
  if ((0x1000 / 4) <= roffset && roffset < (0x1000 / 4 + (PLIC_SOURCE_COUNT + 31) / 32)) {
    int_pend_regs[roffset - 0x1000/4] = *(uint32_t*)dataptr;
  }
  if ((0x2000 / 4) <= roffset && roffset < (0x2000u / 4 + PLIC_CTX_COUNT * (0x20))) {
    int_en_regs[roffset - 0x2000/4] = *(uint32_t*)dataptr & (~1); // cannot enable non-existent interrupt 0
  }
  if (offset >= 0x200000 && ((offset & 0xFFF) == 0)) {
    int_prio_thres[OFFSET_TO_CTX(offset)] = *(uint32_t*)dataptr;
  }
  if (offset >= 0x200000 && ((offset & 0xFFF) == 4)) {
    // interrupt complete
    /*
    uint16_t regnum = *(uint32_t*)dataptr / 32;
    uint16_t regbit = *(uint32_t*)dataptr % 32;
    
    int_pend_regs[regnum] &= ~(1 << regbit);
    */
    int_handling[OFFSET_TO_CTX(offset)] = false;
    plic_notify_finish_ctx(OFFSET_TO_CTX(offset));
  }
}
