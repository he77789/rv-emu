#pragma once
#include <cstdint>

constexpr uint16_t TLB_SIZE = 16;

struct TLBEntry {
  uint64_t virt_page;
  uint64_t phy_page;
  uint64_t pte_addr;
  // size denotes no. of page table layers to ignore
  // handles superpages
  uint8_t size;
  uint8_t permissions;
  bool user;
};
// simple ring buffer
struct TLBStruct {
  uint16_t next_entry;
  TLBEntry tlb_entries[TLB_SIZE];
};

#define PMP_COUNT 16

struct ExpPMP {
  bool enable, lock;
  uint64_t lbound, ubound;
  uint8_t lxwr;
};

struct HartState {
  uint16_t hartid;
  int64_t regs[32];
  uint64_t pc;
  int32_t inst;
  int16_t instbuf;
  uint8_t inst_len;
  //uint64_t csr[4096];
  uint8_t privmode;
  bool mem_status;
  bool page_fault;
  TLBStruct tlb;
  
  // CSRs
  uint64_t mstatus, sstatus;
  
  uint64_t medeleg;
  uint64_t mideleg;
  
  uint64_t mie, sie;
  uint64_t mtvec, stvec;
  
  uint64_t mscratch, sscratch;
  uint64_t mepc, sepc;
  uint64_t mcause, scause;
  uint64_t mtval, stval;
  uint64_t mip; // sip is just a mirror of mip with some bits masked
  
  uint64_t mcycle, minstret;
  
  uint64_t satp;
  
  // RO CSRs
  uint64_t misa;
  uint64_t mhartid;
  
  uint64_t cycle, instret;
  
  // PMP registers
  uint8_t pmpcfg[PMP_COUNT];
  uint64_t pmpaddr[PMP_COUNT];
  bool pmp_lockedaddr[PMP_COUNT]; // bitfield to control locked pmpaddr
  ExpPMP pmp_expanded[PMP_COUNT];
  
  bool chk_int; // signal hart to check interrupts in the next cycle
};

#include "constants.h"

// the sign bit
enum class HartException : int64_t {
  // traps
  IMISALIGN=0, IAFAULT, ILLINST, BPOINT, LMISALIGN, LAFAULT, SMISALIGN, SAFAULT,
  UECALL, HSECALL, VSECALL, MECALL, IPFAULT, LPFAULT, RES2, SPFAULT,
  
  NOEXC=(0b1LL << 63)+16LL, // interrupt 16, platform reserved as no exception
  
  // interrupts
  RES3=(0b1LL << 63), SSOFTINT, RES4, MSOFTINT,
  RES5, STIMEINT, RES6, MTIMEINT,
  RES7, SEXTINT, RES8, MEXTINT
  
};

bool cycle(HartState &hs);

HartException fetch(HartState &hs);
HartException decode_exc(HartState &hs);

enum class inst_type {R, R4, I, S, B, U, J, DIFF}; // DIFF is for opcodes that have multiple possible types
enum class inst_op_32 : uint16_t {
	LOAD=0  ,LOAD_FP ,CST0    ,MISC_MEM,OP_IMM  ,AUIPC   ,OP_IMM_32,L1,
	STORE   ,STORE_FP,CST1    ,AMO     ,OP      ,LUI     ,OP_32    ,L2,
	MADD    ,MSUB    ,NMSUB   ,NMADD   ,OP_FP   ,RES1    ,CST2     ,L3,
	BRANCH  ,JALR    ,RES2    ,JAL     ,SYSTEM  ,RES3    ,CST3     ,L4
};
const inst_type inst_type_32[] = {
	inst_type::I   ,inst_type::DIFF,inst_type::DIFF,inst_type::I   ,inst_type::I   ,inst_type::U   ,inst_type::I   ,inst_type::DIFF,
	inst_type::S   ,inst_type::DIFF,inst_type::DIFF,inst_type::R   ,inst_type::R   ,inst_type::U   ,inst_type::R   ,inst_type::DIFF,
	inst_type::R4  ,inst_type::R4  ,inst_type::R4  ,inst_type::R4  ,inst_type::R   ,inst_type::DIFF,inst_type::DIFF,inst_type::DIFF,
	inst_type::B   ,inst_type::I   ,inst_type::DIFF,inst_type::J   ,inst_type::I   ,inst_type::DIFF,inst_type::DIFF,inst_type::DIFF
};

//bool chk_ill_csr(uint16_t csrno);
bool chk_ro0_csr(uint16_t csrno);

void dump_state(HartState &hs);
void reset_state(HartState &hs, uint16_t hartid);

uint64_t* csr_from_addr(HartState &hs, uint16_t addr);
HartException pmpcfg_rw(HartState &hs, uint16_t addr, uint64_t* rvalue, uint64_t wvalue, uint8_t funct);
