#include <cstdio>
#include <cstdint>
#include <cstring>

#include "cpu.h"
#include "mem.h"
#include "io.h"
#include "constants.h"

uint8_t *main_mem = nullptr;
// variable length based on the number of harts
uint64_t *reservations;
uint8_t dtb_buf[MAX_DTB_SIZE];

std::mutex atomic_op_mtx;

void mem_init(){
  main_mem = new uint8_t[MACH_MEM_SIZE] {0};
  reservations = new uint64_t[MACH_HART_COUNT] {0};
}

void mem_free(){
  delete[] main_mem;
  delete[] reservations;
}

// output format:
// 00L0000XWR<pmpno [5:0]>
// no pmp matched: 0xFFF0
uint16_t chk_pmp(HartState& hs, uint64_t addr){
  uint64_t lower_bound = 0;
  uint64_t upper_bound = 0;
  uint8_t size = 0;
  uint64_t pmp_addr = 0;
  for (uint8_t ind = 0; ind < PMP_COUNT; ind++){
    uint8_t pmp_entry = hs.pmpcfg[ind];
    uint8_t pmp_a = (pmp_entry >> 3) & 0b11;
    if (!pmp_a) continue; // OFF
    if (hs.privmode == 0x3 && !(pmp_entry & (1<<7))) { // not locked for M-mode access, ignore
      continue;
    }
    switch(pmp_a){
      case 1: // TOR
        if (ind == 0){ lower_bound = 0; }
        else { lower_bound = hs.pmpcfg[ind - 1] << 2;} // pmpaddr(N-1)
        upper_bound = hs.pmpcfg[ind] << 2; // pmpaddrN
        break;
      case 2: // NA4
        lower_bound = hs.pmpcfg[ind] << 2;
        upper_bound = lower_bound + 4;
        break;
      case 3: // NAPOT
      /*
        size = 0;
        pmp_addr = hs.csr[0x3B0 + 4 * csrno + csrind];
        size = ffsll(~pmp_addr)-1; // find first 0
        lower_bound = (pmp_addr >> size) << 2;
        upper_bound = lower_bound + (1ULL << (size + 2));
        break;
      */
        size = 0;
        pmp_addr = hs.pmpaddr[ind];
        if (pmp_addr != 0xffffffffffffffff) {
          size = ffsll(~pmp_addr)-1; // find first 0
          lower_bound = (pmp_addr >> size) << (size + 2);
          upper_bound = lower_bound + (1ULL << (size + 2));
        } else { // whole address space
          lower_bound = 0;
          upper_bound = 0xffffffffffffffff;
        }
        break;
    }
    if ((lower_bound <= addr) && (addr < upper_bound)) return (((uint16_t)pmp_entry & 0b10000111) << 6) + ind;
  }
  return 0xFFF0;
}

// checks the range [addrl,addrh] as one access
// output format:
// L0000XWR
// no PMP matched: 0xFF
// range crosses PMP boundary (access fault): 0xF7
uint8_t chk_pmp_range(HartState& hs, uint64_t addrl, uint64_t addrh){
  uint64_t lower_bound = 0;
  uint64_t upper_bound = 0;
  uint8_t size = 0;
  uint64_t pmp_addr = 0;
  bool lowchk, hichk;
  for (uint8_t ind = 0; ind < PMP_COUNT; ind++){
    uint8_t pmp_entry = hs.pmpcfg[ind]; // pmpcfgN
    if (!(pmp_entry & 0b11000)) continue; // OFF
    if (hs.privmode == 0x3 && !(pmp_entry & (1<<7))) { // not locked for M-mode access, ignore
      continue;
    }
    uint8_t pmp_a = (pmp_entry >> 3) & 0b11;
    switch(pmp_a){
      case 1: // TOR
        if (ind == 0){ lower_bound = 0; }
        else { lower_bound = hs.pmpaddr[ind-1] << 2;} // pmpaddr(N-1)
        upper_bound = hs.pmpaddr[ind] << 2; // pmpaddrN
        break;
      case 2: // NA4
        lower_bound = hs.pmpaddr[ind] << 2;
        upper_bound = lower_bound + 4;
        break;
      case 3: // NAPOT
        size = 0;
        pmp_addr = hs.pmpaddr[ind];
        if (pmp_addr != 0xffffffffffffffff) {
          size = ffsll(~pmp_addr)-1; // find first 0
          lower_bound = (pmp_addr >> size) << (size + 2);
          upper_bound = lower_bound + (1ULL << (size + 2));
        } else { // whole address space
          lower_bound = 0;
          upper_bound = 0xffffffffffffffff;
        }
        break;
    }
    lowchk = lower_bound <= addrl && addrl < upper_bound;
    hichk = lower_bound <= addrh && addrh < upper_bound;
    if (lowchk && hichk) return (uint8_t)pmp_entry & 0b10000111; // both bounds satisfied
    if (lowchk ^ hichk) { // only one bound satisfied, the range crosses the boundary
      return 0xF7;
    }
    // did not match, move to next PMP
  }
  return 0xFF;
}

void sync_exp_pmp(HartState& hs) {
  hs.pmp_all_enabled = false;
  hs.min_lbound = 0xffffffffffffffff;
  hs.max_ubound = 0x0;
  for (size_t i = 0; i < PMP_COUNT; i++) {
    const uint8_t raw_pmp_a = (hs.pmpcfg[i] & 0b11000) >> 3;
    if (!raw_pmp_a) { // this PMP is disabled
      hs.pmp_expanded[i].enable = false;
    } else {
      hs.pmp_expanded[i].enable = true;
      hs.pmp_expanded[i].lock = (hs.pmpcfg[i] >> 7) == 1;
      uint8_t size;
      uint64_t pmp_addr;
      switch(raw_pmp_a){
        case 1: // TOR
          if (i == 0){ hs.pmp_expanded[i].lbound = 0; }
          else { hs.pmp_expanded[i].lbound = hs.pmpaddr[i-1] << 2;} // pmpaddr(N-1)
          hs.pmp_expanded[i].ubound = hs.pmpaddr[i] << 2; // pmpaddrN
          break;
        case 2: // NA4
          hs.pmp_expanded[i].lbound = hs.pmpaddr[i] << 2;
          hs.pmp_expanded[i].ubound = hs.pmp_expanded[i].lbound + 4;
          break;
        case 3: // NAPOT
          size = 0;
          pmp_addr = hs.pmpaddr[i];
          if (pmp_addr != 0xffffffffffffffff) {
            size = ffsll(~pmp_addr)-1; // find first 0
            hs.pmp_expanded[i].lbound = (pmp_addr >> size) << (size + 2);
            hs.pmp_expanded[i].ubound = hs.pmp_expanded[i].lbound + (1ULL << (size + 2));
          } else { // whole address space
            hs.pmp_expanded[i].lbound = 0;
            hs.pmp_expanded[i].ubound = 0xffffffffffffffff;
          }
          break;
      }
      hs.pmp_expanded[i].lxwr = hs.pmpcfg[i] & 0b10000111;

      bool is_rwx = hs.pmp_expanded[i].lxwr == 0b111;
      if (hs.pmp_expanded[i].lbound < hs.min_lbound && !is_rwx) hs.min_lbound = hs.pmp_expanded[i].lbound;
      if (hs.pmp_expanded[i].ubound > hs.max_ubound && !is_rwx) hs.max_ubound = hs.pmp_expanded[i].ubound;
      if (hs.pmp_expanded[i].lbound == 0x0 && hs.pmp_expanded[i].ubound == 0xffffffffffffffff && is_rwx) {
        hs.pmp_all_enabled = true;
      }
    }
  }
}

uint16_t chk_pmp_exp(HartState& hs, uint64_t addr) {
  if (hs.pmp_all_enabled) {
    if (addr > hs.max_ubound || addr < hs.min_lbound) {
      return 0b111 << 6; // it is certainly RWX
    }
  }
  for (size_t i = 0; i < PMP_COUNT; i++) {
    const ExpPMP thispmp = hs.pmp_expanded[i];
    if (!thispmp.enable) continue;
    if (hs.privmode == 0x3 && !thispmp.lock) continue;
    if ((thispmp.lbound <= addr) && (addr < thispmp.ubound)) return (thispmp.lxwr << 6) + i;
  }
  return 0xFFF0;
}
uint8_t chk_pmp_range_exp(HartState& hs, uint64_t addrl, uint64_t addrh) {
  if (hs.pmp_all_enabled) {
    if (addrl > hs.max_ubound || addrh < hs.min_lbound) {
      return 0b111; // it is certainly RWX
    }
  }

  for (size_t i = 0; i < PMP_COUNT; i++) {
    const ExpPMP thispmp = hs.pmp_expanded[i];
    if (!thispmp.enable) continue;
    if (hs.privmode == 0x3 && !thispmp.lock) continue;
    bool lowchk = thispmp.lbound <= addrl && addrl < thispmp.ubound;
    bool hichk = thispmp.lbound <= addrh && addrh < thispmp.ubound;
    if (lowchk && hichk) return thispmp.lxwr; // both bounds satisfied
    if (lowchk ^ hichk) { // only one bound satisfied, the range crosses the boundary
      return 0xF7;
    }
  }
  return 0xFF;
}

// sets mem_status and page_fault accordingly; returns 0x0 if an access fault or page fault happens
// caller should save hs.mem_status
// iswrite -> checks pte.W
// mem_status true -> checks pte.R
// mem_status false -> checks pte.X
uint64_t page_table_walk(HartState& hs, uint64_t virt_addr, bool iswrite){
  uint8_t mode = hs.satp >> 60; // satp.MODE
  int8_t level = 0;
  uint8_t ptesize = 0;
  switch (mode){
    case 8:/* Sv39 */
      level = 3;
      ptesize = 8;
      break;
    case 9:/* Sv48 */
      level = 4;
      ptesize = 8;
      break;
    case 10:/* Sv57 */
      level = 5;
      ptesize = 8;
      break;
  }
  if (ptesize == 0) {
    hs.mem_status = false;
    return virt_addr; // Bare mode
  }
  uint64_t a = PAGENUM(hs.satp & 0xFFFFFFFFFFF); // bottom 44 bits of satp, i.e. satp.PPN
  uint64_t pte;
  uint64_t pte_addr;
  bool curr_mem_status = hs.mem_status;
  for (int8_t i = level - 1;i >= 0; i--){
    hs.mem_status = curr_mem_status;
    pte_addr = a + ptesize * ((virt_addr >> (9 * i + 12)) & 0b111111111);
    pte = mem_fetch<uint64_t>(hs, pte_addr);
    if (hs.mem_status) {
      return 0x0; // access fault due to PMP
    }
    if (!(pte & 0b1ULL) /* V bit clear */ || ((pte & 0b110) == 0b100)/* pte.R=0 & pte.W=1 */) {
      hs.page_fault = true;
      return 0x0;
    }
    if (!(pte & 0b1110)){ // pointer to next level
      a = PAGENUM((pte >> 10) & ~(0b111ULL << 51));
      continue;
    }
    // leaf pte
    // check permissions
    if (iswrite) {
      if (!(pte & 0b100)) { // write without W bit
        hs.page_fault = true;
        return 0x0;
      }
    } else if ((curr_mem_status && !(pte & 0b10)) // read without R bit
        || (!curr_mem_status && !(pte & 0b1000
          || ((hs.mstatus & (0b1UL << 19)) && (pte & 0b10)) /* MXR */) ) ) {
      hs.page_fault = true;
      return 0x0;
    }
    
    if (pte & (0b1ULL << 4)) { // U bit handling
      if (hs.privmode >= 0x1) { // S mode
        if (!(hs.mstatus & (0b1ULL << 18)) || (!curr_mem_status && !iswrite)) { // SUM bit not set or we are executing
          hs.page_fault = true;
          return 0x0;
        }
      }
    } else if (hs.privmode == 0x0) { // U mode accessing non-U page, fault
      hs.page_fault = true;
      return 0x0;
    }
    
    uint64_t ppn = (pte & 0x3FFFFFFFFFFC00ULL) << 2; // pte.ppn
    if (i > 0 && (ppn & ~(UINT64_ALL_ONES << (9 * i + 12)) ) ) {hs.page_fault = true; return 0x0;} // misaligned superpage
    
    if (~(pte | (0b11ULL << 6))) { // skip writing if A and D bits are both set
      pte |= 0b1ULL << 6; // pte.A
      if (iswrite) pte |= 0b1ULL << 7; // pte.D
      mem_store<uint64_t>(hs, pte_addr, pte); // store back the modified pte
      if (hs.mem_status) return 0x0;
    }
    
    // translation successful
    uint64_t phy_addr = virt_addr & ~(UINT64_ALL_ONES << (12 + 9*i)); // bottom 12 bits (page offset) + superpage offset
    
    // write translation to TLB
    TLBEntry new_entry = {
        .virt_page = virt_addr & (UINT64_ALL_ONES << (12 + 9*i)),
        .phy_page = ppn,
        .pte_addr = pte_addr,
        .size = (uint8_t)i,
        .permissions = (uint8_t)((pte >> 1) & 0b111),
        .user = (bool)(pte & 0b10000)
    };
    tlb_add(hs.tlb, new_entry);
    
    phy_addr += ppn;
    hs.mem_status = false;
    return phy_addr;
  }
  // if we got here, we didn't find a leaf pte, page fault
  hs.page_fault = true;
  return 0x0;
}

void* dtb_r (uint64_t offset, [[maybe_unused]] uint8_t len) {
  return (uint8_t*)dtb_buf + offset;
}

void dtb_w (uint64_t offset, void* dataptr, uint8_t len) {
  switch (len) {
    case 8:
      dtb_buf[offset] = *(uint8_t*) dataptr;
      break;
    case 16:
      *(uint16_t*)(dtb_buf + offset) = *(uint16_t*) dataptr;
      break;
    case 32:
      *(uint32_t*)(dtb_buf + offset) = *(uint32_t*) dataptr;
      break;
    case 64:
      *(uint64_t*)(dtb_buf + offset) = *(uint64_t*) dataptr;
      break;
   }
}

void* null_r ([[maybe_unused]] uint64_t offset, [[maybe_unused]] uint8_t len) { return &ZERO;}
void null_w ([[maybe_unused]] uint64_t offset, [[maybe_unused]] void* dataptr, [[maybe_unused]] uint8_t len) {return;}

void dump_mem(){
  FILE* f = fopen("mem_dump","wb");
  if (!fwrite(main_mem,1,MACH_MEM_SIZE,f)){
     perror("ERRNO");
     dbg_print("Error dumping memory");
     dbg_endl();
  }
  fclose(f);
}

void tlb_clear(TLBStruct *tlb) {
  memset(tlb, 0, sizeof(TLBStruct));
}

// hash function for the hash table used for the TLB
uint16_t tlb_hash(uint64_t addr) {
  // 106039 / 2^16 is an approximation of the golden ratio
  return ((106039 * addr) >> 16) % TLB_SIZE;
}

// find cached translation in TLB if it exists, returns 0x0 otherwise
uint64_t tlb_find(HartState& hs, uint64_t virt_addr, uint8_t perms) {
  if (!hs.satp) return virt_addr; // virtual memory disabled
  for (int8_t i = hs.tlb.max_entry_size; i >= 0; i--) {
    uint8_t offset_size = 12 + 9 * i;
    TLBEntry curr_entry = hs.tlb.tlb_entries[tlb_hash(virt_addr & (UINT64_ALL_ONES << offset_size))];
    if (i != curr_entry.size) continue; // just a collision of an entry with a different size

    if (!curr_entry.permissions) continue; // perms are all unset, invalid entry
    //if ( (virt_addr & (UINT64_ALL_ONES << offset_size)) == curr_entry.virt_page) {
    if ((virt_addr ^ curr_entry.virt_page) < (1ULL << (offset_size))) {
      if (perms & curr_entry.permissions) { // check permissions
        if (hs.privmode == 0x0 && !curr_entry.user) continue; // U mode accessing S mode pages
        if (hs.privmode >= 0x1 && curr_entry.user && !(hs.mstatus & (0b1ULL << 18)) ) continue; // S mode accessing U mode pages, but SUM bit does not permit this
        // set A and D bits
        uint64_t pte = mem_fetch<uint64_t>(hs,curr_entry.pte_addr);
        if (~(pte | (0b11ULL << 6)) ) { // skip writing if A and D bits are both set
          pte |= 0b1ULL << 6; // A bit
          if (perms & 0b010) pte |= 0b1ULL << 7;
          mem_store<uint64_t>(hs,curr_entry.pte_addr,pte);
          if (hs.mem_status) {
            // the TLB's cached PTE address is inaccessible due to PMP
            // instead of directly throwing an access fault, we just declare it a TLB miss and confirm with a page table walk
            hs.mem_status = false;
            return 0x0;
          }
        }
        
        return curr_entry.phy_page + (virt_addr & (UINT64_ALL_ONES >> (64 - offset_size)) ); // bottom offset_size bits is the (super)page offset
      }
    }
  }
  return 0x0; // TLB miss
}

// hash table of TLB entries
void tlb_add(TLBStruct &tlb, TLBEntry tlb_entry) {
  uint64_t index = tlb_hash(tlb_entry.virt_page);

  TLBEntry old_entry = tlb.tlb_entries[index];
  if (old_entry.permissions != 0) { // we are replacing a valid entry
    tlb.size_count[old_entry.size] -= 1;
  }
  tlb.size_count[tlb_entry.size] += 1;
  tlb.max_entry_size = 0;
  for (uint8_t i = 0; i < 6 ; i++) {
    if (tlb.size_count[i] > 0) tlb.max_entry_size = i;
  }

  tlb.tlb_entries[index] = tlb_entry; // add entry to hash table
}

template <> uint8_t mem_fetch<uint8_t>(HartState& hs, uint64_t addr){
  if (addr < 0x1000 || addr >= (0x8000'0000 + MACH_MEM_SIZE)) {
    hs.mem_status = true;
    return 0x0;
  }
#ifndef DISBALE_PMP
  uint16_t pmp_state = chk_pmp_exp(hs,addr);
  if (pmp_state == 0xFFF0 && hs.privmode <= 0b01){ // no matching PMP in S or U-mode, access fault
    hs.mem_status = true;
    return 0;
  }
  if (hs.mem_status && !(pmp_state & (0b1 << 6))){ // load, but read access disabled, access fault
    hs.mem_status = true;
    return 0;
  }
  if (!(hs.mem_status || (pmp_state & (0b1 << 8)))){ // fetch, but execute access disabled, access fault (de Morgan's laws)
    hs.mem_status = true;
    return 0;
  }
#endif
  hs.mem_status = false;
  return phy_mem_fetch<uint8_t>(addr);
}

template <> void mem_store<uint8_t>(HartState& hs, uint64_t addr, uint8_t data){
  if (addr < 0x1000 || addr >= (0x8000'0000 + MACH_MEM_SIZE)) {
    hs.mem_status = true;
    return;
  }
#ifndef DISABLE_PMP
  uint16_t pmp_state = chk_pmp_exp(hs,addr);
  if (pmp_state == 0xFFF0 && hs.privmode <= 0b01){ // no matching PMP in S or U-mode, access fault
    hs.mem_status = true;
    return;
  }
  if (!(pmp_state & (0b1 << 7))){ // store, but write access disabled, access fault
    hs.mem_status = true;
    return;
  }
#endif
  hs.mem_status = false;
  phy_mem_store<uint8_t>(addr,data);
}
