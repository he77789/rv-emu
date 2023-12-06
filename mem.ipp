#pragma once
#include "constants.h"
#include "mem.h"
#include "io.h"

// the lowest page (0x000 to 0x0FF) are hardcoded protected to fault, to catch errors

// meaning of mem_status:
// when fetch functions are called, mem_status indicates if it's a load (true) or an instruction fetch (false)
// when mem functions return, mem_status indicates if an access fault has occured


template <typename T> T phy_mem_fetch(uint64_t addr){
  if (addr >= 0x8000'0000) [[likely]] {
#ifdef MEM_TRACE
    dbg_print("phy_mem_fetch accessed ");
    dbg_print(addr);
    dbg_endl();
#endif // MEM_TRACE
    void* dptr = main_mem + addr - 0x8000'0000;
    return *reinterpret_cast<T*>(dptr);
  }
  for (int8_t i = mem_entries - 1; i >= 0; i--) {
    int64_t offset = addr - mem_map[i].base;
    if (0 <= offset && offset < mem_map[i].size) {
      return *(T*)mem_map[i].handler_r(offset,sizeof(T));
    }
  }
  if (0x1000 <= addr && addr < (0x1000 + BOOT_ROM_LEN)) return *reinterpret_cast<const T*>(boot_rom_content + addr - 0x1000);
  dbg_print("No matching memory map for ");
  dbg_print(addr);
  dbg_endl();
  return 0xFF;
}

template <typename T> void phy_mem_store(uint64_t addr, T data){
  if (addr >= 0x8000'0000) [[likely]] {
    void* dptr = main_mem + addr - 0x8000'0000;
    *reinterpret_cast<T*>(dptr) = data;
#ifdef MEM_TRACE
    dbg_print("phy_mem_store accessed ");
    dbg_print(addr);
    dbg_endl();
#endif // MEM_TRACE
    return;
  }
  for (int8_t i = 0; i < mem_entries; i++) {
    int64_t offset = addr - mem_map[i].base;
    if (0 <= offset && offset < mem_map[i].size) {
      mem_map[i].handler_w(offset,&data,sizeof(T));
      return;
    }
  }
  /* Boot ROM is unwritable*/
  dbg_print("No matching memory map for ");
  dbg_print(addr);
  dbg_endl();
}

template <typename T> T mem_fetch(HartState& hs, uint64_t addr){
  if (addr < 0x1000 || addr >= (0x8000'0000 + MACH_MEM_SIZE)) {
    hs.mem_status = true;
    return 0;
  }
#ifndef DISABLE_PMP
  uint8_t pmp_state = chk_pmp_range_exp(hs, addr, addr + sizeof(T) - 1);
  if (pmp_state == 0xF7){ /* memory access overlaps the boundary of a PMP, access fault */
    hs.mem_status = true;
    return 0;
  }
  if (pmp_state == 0xFF && hs.privmode < 0b11){ /* no matching PMP for S or U-mode, access fault */
    hs.mem_status = true;
    return 0;
  }
  if (hs.mem_status && !(pmp_state & (0b1 << 0))){ /* load, but read access disabled, access fault */
    hs.mem_status = true;
    return 0;
  }
  if (!(hs.mem_status || (pmp_state & (0b1 << 2)))){ /* fetch, but execute access disabled, access fault (de Morgan's laws) */
    hs.mem_status = true;
    return 0;
  }
#endif // DISABLE_PMP
  hs.mem_status = false;
  return phy_mem_fetch<T>(addr);
}
template <typename T> void mem_store(HartState& hs, uint64_t addr, T data){
  if (addr < 0x1000 || addr >= (0x8000'0000 + MACH_MEM_SIZE)) {
    hs.mem_status = true;
    return;
  }
#ifndef DISABLE_PMP
  uint8_t pmp_state = chk_pmp_range_exp(hs, addr, addr + sizeof(T) - 1);
  if (pmp_state == 0xF7){ /* memory access overlaps the boundary of a PMP, access fault */
    hs.mem_status = true;
    return;
  }
  if (pmp_state == 0xFF && hs.privmode < 0b11){ /* no matching PMP in S or U-mode, access fault */
    hs.mem_status = true;
    return;
  }
  if (!(pmp_state & (0b1 << 1))){ /* store, but write access disabled, access fault */
    hs.mem_status = true;
    return;
  }
#endif // DISABLE_PMP
  hs.mem_status = false;
  phy_mem_store<T>(addr,data);
}

// in mem.cpp
template <> uint8_t mem_fetch<uint8_t>(HartState& hs, uint64_t addr);
template <> void mem_store<uint8_t>(HartState& hs, uint64_t addr, uint8_t data);

// mem_status indicates if it's a load (true) or an instruction fetch (false)
// if an access fault happens, mem_status is set to true
// if a page fault happens, page_fault is set to true
template <typename T> T virt_mem_fetch(HartState& hs, uint64_t addr){
  bool inMPRV = false;
  if (hs.privmode == 0b11) {
    if (!(hs.mem_status && (hs.mstatus & (0b1ULL << 17) && ((hs.mstatus >> 11) & 0b11) != 0b11) ) ) /* MPRV bit check */ {
      return mem_fetch<T>(hs,addr); /* virtual memory is disabled in M-mode*/
    }
    // MPRV is active! setup
    inMPRV = true;
    hs.privmode = 0b11 & (hs.mstatus >> 11);
  }
  uint64_t phy_addr = tlb_find(hs, addr, hs.mem_status ? 0b001 : 0b100);
  if (!phy_addr) {
    bool curr_mem_status = hs.mem_status;
    phy_addr = page_table_walk(hs,addr,false);
    if (hs.page_fault || hs.mem_status) {
      if (inMPRV) hs.privmode = 0b11; // restore priviledge
      return 0x0;
    }
    hs.mem_status = curr_mem_status;
  }
  T output = mem_fetch<T>(hs,phy_addr);
  //hs.mem_status = false;
  if (inMPRV) hs.privmode = 0b11; // restore priviledge
  return output;
}
template <typename T> void virt_mem_store(HartState& hs, uint64_t addr, T data){
  bool inMPRV = false;
  if (hs.privmode == 0b11) {
    if (!(hs.mstatus & (0b1ULL << 17) && ((hs.mstatus >> 11) & 0b11) != 0b11) ) /* MPRV bit check */ {
      mem_store<T>(hs,addr,data); /* virtual memory is disabled in M-mode*/
      return;
    }
    // MPRV is active! setup
    inMPRV = true;
    hs.privmode = 0b11 & (hs.mstatus >> 11);
  }
  uint64_t phy_addr = tlb_find(hs, addr, 0b010);
  if (!phy_addr) {
    bool curr_mem_status = hs.mem_status;
    phy_addr = page_table_walk(hs,addr,true);
    if (hs.page_fault || hs.mem_status) {
      if (inMPRV) hs.privmode = 0b11; // restore priviledge
      return;
    }
    hs.mem_status = curr_mem_status;
  }
  mem_store<T>(hs,phy_addr,data);
  //hs.mem_status = false;
  if (inMPRV) hs.privmode = 0b11; // restore priviledge
}
