#include <cstdint>
#include <chrono>
#include <cstring>

#include "aclint.h"
#include "constants.h"
#include "hartexc.h"

uint64_t time_start = 0;

#ifdef SLOW_MTIMER
uint64_t slow_time = 0;
#endif // SLOW_TIMER

uint64_t* mtimer_regs;
uint32_t* mswi_regs;
uint32_t* sswi_regs;

uint64_t handler_output;

void aclint_mtimer_init() {
  aclint_mtimer_reset();
  mtimer_regs = new uint64_t[MACH_HART_COUNT];
  memset((void*)mtimer_regs,0xff, MACH_HART_COUNT * sizeof(uint64_t)); // set all the registers to their max value, so that it won't trigger until requested
}

void aclint_mtimer_reset() {
  time_start = readtime();
}

void aclint_mswi_init() {
  mswi_regs = new uint32_t[MACH_HART_COUNT];
}

void aclint_sswi_init() {
  sswi_regs = new uint32_t[MACH_HART_COUNT];
}

uint64_t aclint_mtime_get() {
  return (readtime() - time_start) / 100; // nanos since power on divided by 100 to get a 10MHz clock
}

void* aclint_mtimer_r (uint64_t offset, [[maybe_unused]] uint8_t len) {
  if (offset == 0x7ff8) {
    handler_output = aclint_mtime_get();
  } else {
    handler_output = mtimer_regs[offset];
  }
  return (void*) &handler_output;
}
void aclint_mtimer_w (uint64_t offset, void* dataptr, [[maybe_unused]] uint8_t len) {
  if (offset == 0x7ff8) {
    time_start = readtime() - *(uint64_t*)dataptr * 100;
  } else {
    mtimer_regs[offset] = *(uint64_t*)dataptr;
    // mtimecmp updated, we should check if the interrupt is still pending
    aclint_mtimer_chk(hartlist[offset]);
  }
}

void aclint_mtimer_chk(HartState& hs) {
#ifdef SLOW_MTIMER
  slow_time++;
#endif // SLOW_MTIMER
  //if (hs.mip & (1 << 7)) return; // the hart already has an interrupt pending, we do not need to trigger
  if (mtimer_regs[hs.hartid] <= aclint_mtime_get()) {
    hs.mip |= 1 << 7; // MTIP
    //setup_pending_int(hs);
  } else {
    hs.mip &= ~(1 << 7);
  }
}

uint64_t readtime(){
#ifdef SLOW_MTIMER
  return slow_time;
#else
  using namespace std::chrono;
  return duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
#endif // SLOW_MTIMER
}

void* aclint_mswi_r (uint64_t offset, [[maybe_unused]] uint8_t len) {
  return &mswi_regs[offset];
}
void aclint_mswi_w (uint64_t offset, void* dataptr, [[maybe_unused]] uint8_t len) {
  mswi_regs[offset] = *(uint64_t*)dataptr & 0b1;
}
void aclint_mswi_chk(HartState& hs) {
  if (mswi_regs[hs.hartid]) {
    hs.mip |= 1 << 3; // MSIP
  }
}
