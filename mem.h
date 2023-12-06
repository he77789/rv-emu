#pragma once
#include <cstdint>
#include <mutex>

#include "cpu.h"
#include "constants.h"

extern uint8_t *main_mem;
extern uint64_t *reservations;
extern uint8_t dtb_buf[MAX_DTB_SIZE];
extern std::mutex atomic_op_mtx;
void mem_init();
void mem_free();

// direcly check PMP accesses from raw PMP registers
uint16_t chk_pmp(HartState& hs, uint64_t addr);
uint8_t chk_pmp_range(HartState& hs, uint64_t addrl, uint64_t addrh);

// sync expanded PMP entries with raw PMP registers
void sync_exp_pmp(HartState& hs);

// check PMP accesses with expanded PMP entries
uint16_t chk_pmp_exp(HartState& hs, uint64_t addr);
uint8_t chk_pmp_range_exp(HartState& hs, uint64_t addrl, uint64_t addrh);

// convert a physical 4KiB page number to a physical address
#define PAGENUM(x) ((x)<<12)

uint64_t page_table_walk(HartState& hs, uint64_t virt_addr, bool iswrite);

// handler_r should return the correct pointer given the requested length
struct Memmap_Entry {
  uint64_t base; int64_t size;
  void* (*handler_r) (uint64_t offset, uint8_t len);
  void (*handler_w) (uint64_t offset, void* dataptr, uint8_t len);
};

#include "aclint.h"
#include "uart.h"
#include "plic.h"
#include "virtio_mmio_blk.h"

// physical memory map:
// 0x8000'0000: RAM
// 0x1000'1000: virtio mmio disk
// 0x1000'0000: NS16550A UART
// 0xC00'0000: PLIC
// 0x200'4000: ACLINT MTIMER
// 0x200'0000: ACLINT MSWI
// 0x1100: dtb file
// 0x1000: Boot ROM

void* dtb_r (uint64_t offset, [[maybe_unused]] uint8_t len);
void dtb_w (uint64_t offset, void* dataptr, uint8_t len);

// unused params for callback format
void* null_r ([[maybe_unused]] uint64_t offset, [[maybe_unused]] uint8_t len);
void null_w ([[maybe_unused]] uint64_t offset, [[maybe_unused]] void* dataptr, [[maybe_unused]] uint8_t len);
static const Memmap_Entry mem_map[] = {
  {0x1000'1000,0x1000,virtio_mmio_blk_r,virtio_mmio_blk_w}, // TODO: virtio mmio disk
  {0x1000'0000,16,uart_r,uart_w},
  {0xC00'0000,0x400'0000,plic_r,plic_w},
  {0x200'4000,0x8000,aclint_mtimer_r,aclint_mtimer_w},
  {0x200'0000,0x4000,aclint_mswi_r,aclint_mswi_w},
  {0x1100,MAX_DTB_SIZE,dtb_r,dtb_w}
};
constexpr uint8_t mem_entries = sizeof(mem_map) / sizeof(Memmap_Entry);

// simply jump to 0x8000'0000
constexpr uint16_t BOOT_ROM_LEN = 12;
static const uint8_t boot_rom_content[BOOT_ROM_LEN] = {
  0x13, 0x04, 0x10, 0x00, // addi x8, x0, 1
  0x13, 0x14, 0xf4, 0x01, // slli x8, x8, 31
  0x67, 0x00, 0x04, 0x00  // jalr x0, x8, 0x0
};

void dump_mem();

// TLB handling functions
void tlb_clear(TLBStruct *tlb);
uint64_t tlb_find(HartState& hs, uint64_t virt_addr, uint8_t perms);
void tlb_add(TLBStruct &tlb, TLBEntry tlb_entry);

// templated memory access implementation
#include "mem.ipp"
