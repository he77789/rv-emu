#include <cstring>
#include <cstdint>
#include <mutex>
#include <thread>

#include "constants.h"
#include "cpu.h"
#include "mem.h"
#include "io.h"
#include "hartexc.h"
#include "aclint.h"

// for MULH and friends
#ifdef __SIZEOF_INT128__
typedef __int128_t int128_t;
typedef __uint128_t uint128_t;
#else
#define NOINT128
#endif

// for change by debuggers
volatile uint64_t breakpoint = 0;

// for signature
bool sig_mode=false;
uint64_t begin_signature;
uint64_t end_signature;

// false to halt
bool cycle(HartState &hs){
  /*
  if (hs.pc == 0) {
    dbg_print("PC at 0! Halting.");
    dbg_endl();
    return false;
  }
  */
  if (hs.pc == breakpoint) {
    dbg_print("breakpoint reached");
    dbg_endl();
    ;
  }

  HartException exc;
  hs.regs[0] = 0;
  exc = fetch(hs);
  /*
  if (hs.inst_len == 0){
    dbg_print("zero instruction!\n");
    dump_state(hs);
    return false;
  }
  */
  if (exc != HartException::NOEXC) {
    goto cycle_end;
  }
  
  #ifdef PC_TRACE
  dbgerr_print(hs.pc);
  dbgerr_endl();
  #endif
  
  exc = decode_exc(hs);
  /*
  if (hs.inst_len == 0){
    dbg_print("zero instruction!\n");
    dump_state(hs);
    return false;
  }
  */
  if (exc != HartException::NOEXC) {
    if (exc == HartException::BPOINT && sig_mode) {
      return false;
    }
    goto cycle_end;
  }
  
cycle_end:
  hs.mcycle++; // mcycle CSR
  
  // mirror mcycle and minstret to cycle and instret CSR
  hs.cycle = hs.mcycle;
  hs.instret = hs.minstret;
  
  // check for mip, sip
  if (hs.chk_int) {
    hs.chk_int = false;
    setup_pending_int(hs);
  }
  
  return true;
}

#define HANDLE_MEM_ERROR(X,tval) \
if (hs.mem_status){\
  hs.mem_status = false;\
  return create_exception(hs,HartException::X##AFAULT, tval);\
}\
if (hs.page_fault){\
  hs.page_fault = false;\
  return create_exception(hs,HartException::X##PFAULT, tval);\
}

HartException fetch(HartState &hs){
  hs.mem_status = false;
  if (!hs.instbuf){
    hs.inst = (uint32_t)virt_mem_fetch<uint32_t>(hs,hs.pc);
  } else {
    hs.inst = hs.instbuf;
    hs.instbuf = 0;
  }
  HANDLE_MEM_ERROR(I, hs.pc)
  
  // instruction of all zeros or ones, guaranteed to be illegal in the spec
  if (hs.inst == 0 || (uint32_t)hs.inst == 0xffffffff) {hs.inst_len = 0; return create_exception(hs,HartException::ILLINST, hs.inst);}
  
  // check instruction length
  if (!(hs.inst & 0b11)){
    hs.inst_len = 16;
  } else if ((hs.inst & 0b11100) != 0b11100){
    hs.inst_len = 32;
  } else {
    dbg_print("unsupported instruction length");
    dbg_endl();
    hs.inst_len = 0;
    return create_exception(hs,HartException::IMISALIGN, hs.pc);
  }
  
  // handle instruction length
  switch (hs.inst_len){
    case 16: // split into two instructions
      hs.instbuf = hs.inst >> 16;
      hs.inst &= 0xFFFF;
      break;
    case 32: // the instruction is already in the right place
      break;
    default:
      return create_exception(hs,HartException::IMISALIGN, hs.pc);
  }
  HANDLE_MEM_ERROR(I, hs.pc)
  return HartException::NOEXC;
}

HartException decode_exc(HartState &hs){
  // decode
  uint8_t opcode = hs.inst & 0x0000007f; // 6-0 bits
  
  inst_op_32 op_fun = (inst_op_32)0b11111;
  inst_type op_type = inst_type::DIFF;
  uint8_t rd = 0, rs1 = 0, rs2 = 0/*, rs3 = 0*/;
  uint8_t /*funct2 = 0,*/ funct3 = 0, funct7 = 0;
  int64_t imm = 0;
  
  switch (hs.inst_len){
    case 32:
      op_fun = (inst_op_32)((opcode >> 2) & 0b11111);
      op_type = inst_type_32[(uint16_t)op_fun];
      
      rd = (hs.inst >> 7) & 0b11111;
      rs1 = (hs.inst >> 15) & 0b11111; 
      funct3 = (hs.inst >> 12) & 0b111;
      switch (op_type){
        
        case inst_type::R4:
          // R4 is not used in RV64IMA
          /*
          rs3 = (hs.inst >> 27) & 0b11111;
          funct2 = (hs.inst >> 25) & 0b11;
          */
          rs2 = (hs.inst >> 20) & 0b11111;
          break;
        case inst_type::R:
          rs2 = (hs.inst >> 20) & 0b11111;
          funct7 = (hs.inst >> 25) & 0b1111111;
          break;
        case inst_type::I:
          imm = (hs.inst >> 20);
          break;
        case inst_type::S:
          rs2 = (hs.inst >> 20) & 0b11111;
          // shifting up and then shifting down to sign extend
          imm |= hs.inst & (0b1111111 << 25);
          imm >>= 20;
          imm |= rd;
          break;
        case inst_type::B:
          rs2 = (hs.inst >> 20) & 0b11111;
          // shifting up and then shifting down to sign extend
          imm |= hs.inst & (0b1111111 << 25);
          imm >>= 20;
          imm |= rd;
          if (imm & 1){
            imm |= 0b1 << 11;
          } else {
            imm &= ~(0b1 << 11);
          }
          imm &= ~(0b1);
          break;
        case inst_type::U:
          imm = hs.inst & ~(0b111111111111);
          break;
        case inst_type::J:
          imm = (hs.inst & ~(0b111111111111)) >> 20;
          imm &=  ~(0b11111111 << 12); // mask out [19:12]
          imm |= hs.inst & (0b11111111 << 12); // copy in [19:12]
          if (imm & 1){
            imm |= 0b1 << 11;
          } else {
            imm &= ~(0b1 << 11);
          }
          imm &= ~(0b1);
          break;
        case inst_type::DIFF:
          break;
      };
      break;
    default:
      dbg_print("unsupported instruction length decode:");
      dbg_print(hs.inst_len,false);
      dbg_endl();
      hs.inst_len = 0;
  };
  
  // execute
  /*
  dbg_print(op_fun);
  dbg_endl();
  dbg_print(rd);
  dbg_endl();
  dbg_print(rs1);
  dbg_endl();
  dbg_print(rs2);
  dbg_endl();
  dbg_print(imm);
  dbg_endl();
  dbg_print(funct3);
  dbg_endl();
  */
  uint64_t unsimm = (uint64_t) imm;
  uint64_t swap_tmp = 0;
#ifndef NOINT128
  int128_t full_result = 0;
  //uint128_t u_full_result = 0;
#endif
  bool pc_chgd = false; // true if pc changed by execution, disables pc increment
  
  switch (hs.inst_len){
    case 32:
      switch (op_fun){
        // in the order shown in the risc-v spec version 20191213 document
        
        case inst_op_32::OP_IMM:
          switch (funct3){
            case 0b000:
              hs.regs[rd] = hs.regs[rs1] + imm;
              break;
            case 0b011:
              hs.regs[rd] = (uint64_t)hs.regs[rs1] < unsimm;
              break;
            case 0b010:
              hs.regs[rd] = hs.regs[rs1] < imm;
              break;
            
            case 0b100:
              hs.regs[rd] = hs.regs[rs1] ^ imm;
              break;
            case 0b110:
              hs.regs[rd] = hs.regs[rs1] | imm;
              break;
            case 0b111:
              hs.regs[rd] = hs.regs[rs1] & imm;
              break;
            
            case 0b001:
              hs.regs[rd] = hs.regs[rs1] << (imm & 0b111111);
              break;
            case 0b101:
              if (hs.inst & (0b1 << 30)){
                hs.regs[rd] = hs.regs[rs1] >> (imm & 0b111111); // arithmetic shift
              } else {
                hs.regs[rd] = ((uint64_t)hs.regs[rs1]) >> (imm & 0b111111); // logical shift
              }
              break;
          };
          break;
        case inst_op_32::OP_IMM_32:
          switch (funct3){
            case 0b000:
              hs.regs[rd] = (int32_t)(hs.regs[rs1] + imm);
              break;
            case 0b011:
              hs.regs[rd] = (uint32_t)hs.regs[rs1] < unsimm;
              break;
            case 0b010:
              hs.regs[rd] = (int32_t)hs.regs[rs1] < imm;
              break;
            
            case 0b100:
              hs.regs[rd] = (int32_t)hs.regs[rs1] ^ imm;
              break;
            case 0b110:
              hs.regs[rd] = (int32_t)hs.regs[rs1] | imm;
              break;
            case 0b111:
              hs.regs[rd] = (int32_t)hs.regs[rs1] & imm;
              break;
            
            case 0b001:
              hs.regs[rd] = (int32_t)hs.regs[rs1] << (imm & 0b11111);
              break;
            case 0b101:
              if (hs.inst & (0b1 << 30)){
                hs.regs[rd] = (int32_t)hs.regs[rs1] >> (imm & 0b11111); // arithmetic shift
              } else {
                // N.B. uint32_t -> int32_t -> int64_t sign-extends correctly, but uint32_t -> int64_t directly doesn't work
                hs.regs[rd] = (int32_t)(((uint32_t)hs.regs[rs1]) >> (imm & 0b11111)); // logical shift
              }
              break;
          };
          break;
        // U-mode instructions have their immediate already shifted left 12 bits
        case inst_op_32::LUI:
          hs.regs[rd] = imm;
          break;
        case inst_op_32::AUIPC:
          hs.regs[rd] = imm + hs.pc;
          break;
        
        case inst_op_32::OP:
          switch (funct7){
            // I base
            case 0b0100000:
              [[fallthrough]];
            case 0b0000000:
              switch (funct3){
                  case 0b000:
                    if (hs.inst & (0b1 << 30)){
                      hs.regs[rd] = hs.regs[rs1] - hs.regs[rs2];
                    } else {
                      hs.regs[rd] = hs.regs[rs1] + hs.regs[rs2];
                    }
                    break;
                  case 0b011:
                    hs.regs[rd] = (uint64_t)hs.regs[rs1] < (uint64_t)hs.regs[rs2];
                    break;
                  case 0b010:
                    hs.regs[rd] = hs.regs[rs1] < hs.regs[rs2];
                    break;
                  
                  case 0b100:
                    hs.regs[rd] = hs.regs[rs1] ^ hs.regs[rs2];
                    break;
                  case 0b110:
                    hs.regs[rd] = hs.regs[rs1] | hs.regs[rs2];
                    break;
                  case 0b111:
                    hs.regs[rd] = hs.regs[rs1] & hs.regs[rs2];
                    break;
                  
                  case 0b001:
                    hs.regs[rd] = hs.regs[rs1] << (hs.regs[rs2] & 0b111111);
                    break;
                  case 0b101:
                    if (hs.inst & (0b1 << 30)){
                      hs.regs[rd] = hs.regs[rs1] >> (hs.regs[rs2] & 0b111111); // arithmetic shift
                    } else {
                      hs.regs[rd] = ((uint64_t)hs.regs[rs1]) >> (hs.regs[rs2] & 0b111111); // logical shift
                    }
                    break;
                };
                break;
              case 0b0000001: // M extension
                switch (funct3){
                  case 0b000:
                    hs.regs[rd] = hs.regs[rs1] * hs.regs[rs2];
                    break;
                #ifndef NOINT128
                  case 0b001:
                    full_result = (int128_t)hs.regs[rs1] * (int128_t)hs.regs[rs2];
                    hs.regs[rd] = (full_result >> 64);
                    break;
                  case 0b010:
                    // int64_t -> uint64_t -> uint128_t correctly does not sign extend, but int64_t -> uint128_t sign extends
                    full_result = (int128_t)hs.regs[rs1] * (uint128_t)(uint64_t)hs.regs[rs2];
                    hs.regs[rd] = (full_result >> 64);
                    break;
                  case 0b011:
                    full_result = (uint128_t)(uint64_t)hs.regs[rs1] * (uint128_t)(uint64_t)hs.regs[rs2];
                    hs.regs[rd] = (full_result >> 64);
                    break;
                #else // NOINT128 is defined!
                // TODO: implement fallback when 128-bit ints are not available
                #error __int128 is required! Use g++ on a 64-bit system.
                #endif // NOINT128
                  case 0b100:
                    if (hs.regs[rs2] == 0) {
                      hs.regs[rd] = -1;
                    } else if (hs.regs[rs1] == INT64_MIN && hs.regs[rs2] == -1) { // signed division overflow
                      hs.regs[rd] = hs.regs[rs1];
                    } else {
                      hs.regs[rd] = hs.regs[rs1] / hs.regs[rs2];
                    }
                    break;
                  case 0b101:
                    if (hs.regs[rs2] == 0) {
                      hs.regs[rd] = UINT64_MAX;
                    } else {
                      hs.regs[rd] = ((uint64_t)hs.regs[rs1]) / ((uint64_t)hs.regs[rs2]);
                    }
                    break;
                  
                  case 0b110:
                    if (hs.regs[rs2] == 0) {
                      hs.regs[rd] = hs.regs[rs1];
                    } else if (hs.regs[rs1] == INT64_MIN && hs.regs[rs2] == -1) { // signed division overflow
                      hs.regs[rd] = 0;
                    } else {
                      hs.regs[rd] = hs.regs[rs1] % hs.regs[rs2];
                    }
                    break;
                  case 0b111:
                    if (hs.regs[rs2] == 0) {
                      hs.regs[rd] = hs.regs[rs1];
                    } else {
                      hs.regs[rd] = ((uint64_t)hs.regs[rs1]) % ((uint64_t)hs.regs[rs2]);
                    }
                    break;
                };
            };
            break;
          case inst_op_32::OP_32:
            switch (funct7){
              case 0b0100000:
                [[fallthrough]];
              case 0b0000000: // I base
                switch (funct3){
                  case 0b000:
                    if (hs.inst & (0b1 << 30)) {
                      hs.regs[rd] = (int32_t)hs.regs[rs1] - (int32_t)hs.regs[rs2];
                    } else {
                      hs.regs[rd] = (int32_t)hs.regs[rs1] + (int32_t)hs.regs[rs2];
                    }
                    break;
                  case 0b011:
                    hs.regs[rd] = (uint32_t)hs.regs[rs1] < (uint32_t)hs.regs[rs2];
                    break;
                  case 0b010:
                    hs.regs[rd] = (int32_t)hs.regs[rs1] < (int32_t)hs.regs[rs2];
                    break;
                  
                  case 0b100:
                    hs.regs[rd] = (int32_t)hs.regs[rs1] ^ (int32_t)hs.regs[rs2];
                    break;
                  case 0b110:
                    hs.regs[rd] = (int32_t)hs.regs[rs1] | (int32_t)hs.regs[rs2];
                    break;
                  case 0b111:
                    hs.regs[rd] = (int32_t)hs.regs[rs1] & (int32_t)hs.regs[rs2];
                    break;
                  
                  case 0b001:
                    hs.regs[rd] = (int32_t)hs.regs[rs1] << ((int32_t)hs.regs[rs2] & 0b11111);
                    break;
                  case 0b101:
                    if (hs.inst & (0b1 << 30)){
                      hs.regs[rd] = (int32_t)hs.regs[rs1] >> (hs.regs[rs2] & 0b11111); // arithmetic shift
                    } else {
                      // N.B. uint32_t -> int32_t -> int64_t sign-extends correctly, but uint32_t -> int64_t directly doesn't work
                      hs.regs[rd] = (int32_t)(((uint32_t)hs.regs[rs1]) >> (hs.regs[rs2] & 0b11111)); // logical shift
                    }
                    break;
                };
                break;
              case 0b0000001: // M extension
                switch (funct3){
                  case 0b000:
                    hs.regs[rd] = (int32_t)hs.regs[rs1] * (int32_t)hs.regs[rs2];
                    break;

                   case 0b100:
                    if ((int32_t)hs.regs[rs2] == 0) {
                      hs.regs[rd] = (int32_t)-1;
                    } else if (hs.regs[rs1] == INT32_MIN && hs.regs[rs2] == -1) { // signed division overflow
                      hs.regs[rd] = hs.regs[rs1];
                    } else {
                      hs.regs[rd] = (int32_t)hs.regs[rs1] / (int32_t)hs.regs[rs2];
                    }
                    break;
                  case 0b101:
                    if ((uint32_t)hs.regs[rs2] == 0) {
                      hs.regs[rd] = UINT64_MAX; // it's going to be sign-extended anyways
                    } else {
                      hs.regs[rd] = (int32_t)(((uint32_t)hs.regs[rs1]) / ((uint32_t)hs.regs[rs2]));
                    }
                    break;
                  
                  case 0b110:
                    if ((int32_t)hs.regs[rs2] == 0) {
                      hs.regs[rd] = hs.regs[rs1];
                    } else if (hs.regs[rs1] == INT32_MIN && hs.regs[rs2] == -1) { // signed division overflow
                      hs.regs[rd] = 0;
                    } else {
                      hs.regs[rd] = (int32_t)hs.regs[rs1] % (int32_t)hs.regs[rs2];
                    }
                    break;
                  case 0b111:
                    if ((uint32_t)hs.regs[rs2] == 0) {
                      hs.regs[rd] = hs.regs[rs1];
                    } else {
                      hs.regs[rd] = (int32_t)(((uint32_t)hs.regs[rs1]) % ((uint32_t)hs.regs[rs2]));
                    }
                    break;
                };
                break;
            };
            break;
          case inst_op_32::JAL:
            hs.regs[rd] = hs.pc + hs.inst_len / 8;
            hs.pc += imm;
            pc_chgd = true;
            if (hs.pc & 1) { // misaligned jump
              return create_exception(hs,HartException::IMISALIGN,hs.pc);
            }
            break;
            
          case inst_op_32::JALR:
            
            #ifdef CALL_TRACE
            dbg_print("JALR from ");
            dbg_print(hs.pc);
            #endif
            
            // CAUTION: if rd == rs1, this can be a swap
            // so that's why swap_tmp needs to exist
            swap_tmp = hs.pc + hs.inst_len / 8;
            hs.pc = ((hs.regs[rs1] + imm) & ~(0b1));
            hs.regs[rd] = swap_tmp;
            
            #ifdef CALL_TRACE
            dbg_print(" to ");
            dbg_print(hs.pc);
            dbg_endl();
            #endif
            
            pc_chgd = true;
            if (hs.pc & 1) { // misaligned jump
              return create_exception(hs,HartException::IMISALIGN,hs.pc);
            }
            
            break;
          case inst_op_32::BRANCH:
            {
              bool jflag = false;
              switch (funct3){
                case 0b000:
                  if (hs.regs[rs1] == hs.regs[rs2]) jflag = true;
                  break;
                case 0b001:
                  if (hs.regs[rs1] != hs.regs[rs2]) jflag = true;
                  break;
                case 0b100:
                  if (hs.regs[rs1] < hs.regs[rs2]) jflag = true;
                  break;
                case 0b101:
                  if (hs.regs[rs1] >= hs.regs[rs2]) jflag = true;
                  break;
                case 0b110:
                  if ((uint64_t)hs.regs[rs1] < (uint64_t)hs.regs[rs2]) jflag = true;
                  break;
                case 0b111:
                  if ((uint64_t)hs.regs[rs1] >= (uint64_t)hs.regs[rs2]) jflag = true;
                  break;
              }
              if (jflag) {
                hs.pc += imm;
                pc_chgd = true;
                if (hs.pc & 1) { // misaligned jump
                  return create_exception(hs,HartException::IMISALIGN,hs.pc);
                }
              }
            }
            break;
          case inst_op_32::LOAD:
            {
              uint64_t load_addr = hs.regs[rs1] + imm;
              uint64_t lv = 0;
              if (funct3 & 0b100) {
                hs.mem_status = true;
                switch (funct3 - 0b100){
                  case 0b00:
                    lv = virt_mem_fetch<uint8_t>(hs,load_addr);
                    break;
                  case 0b01:
                    lv = virt_mem_fetch<uint16_t>(hs,load_addr);
                    break;
                  case 0b10:
                    lv = virt_mem_fetch<uint32_t>(hs,load_addr);
                    break;
                  case 0b11:
                    lv = virt_mem_fetch<uint64_t>(hs,load_addr);
                    break;
                }
                HANDLE_MEM_ERROR(L, load_addr)
              } else {
                hs.mem_status = true;
                switch (funct3){
                  case 0b00:
                    lv = (int8_t)virt_mem_fetch<uint8_t>(hs,load_addr);
                    break;
                  case 0b01:
                    lv = (int16_t)virt_mem_fetch<uint16_t>(hs,load_addr);
                    break;
                  case 0b10:
                    lv = (int32_t)virt_mem_fetch<uint32_t>(hs,load_addr);
                    break;
                  case 0b11:
                    lv = (int64_t)virt_mem_fetch<uint64_t>(hs,load_addr);
                    break;
                }
                HANDLE_MEM_ERROR(L, load_addr)
                /*
                uint8_t rem_amt = (1<<4) - (1<<funct3);
                lv = ((int64_t)lv << rem_amt) >> rem_amt; // shift the value up and down to sign-extend
                */
              }
              hs.regs[rd] = lv; // the destination register is only modified if the load actually succeeds
            }
            break;
          case inst_op_32::STORE:
            {
              uint64_t store_addr = hs.regs[rs1] + imm;
              hs.mem_status = false;
              switch (funct3){
                case 0b00:
                  virt_mem_store<uint8_t>(hs,store_addr,hs.regs[rs2]);
                  break;
                case 0b01:
                  virt_mem_store<uint16_t>(hs,store_addr,hs.regs[rs2]);
                  break;
                case 0b10:
                  virt_mem_store<uint32_t>(hs,store_addr,hs.regs[rs2]);
                  break;
                case 0b11:
                  virt_mem_store<uint64_t>(hs,store_addr,hs.regs[rs2]);
                  break;
                }
                HANDLE_MEM_ERROR(S, store_addr)
            }
            break;
          case inst_op_32::MISC_MEM:
            // currently, memory accesses are synchronous, so FENCE and FENCE.I instructions are nop
            break;
            
          case inst_op_32::SYSTEM:
            // some SYSTEM instructions are R-type instead of I-type
            rs2 = (hs.inst >> 20) & 0b11111;
            funct7 = (hs.inst >> 25) & 0b1111111;
            
            if (hs.inst == 0b000000000000'00000'000'00000'1110011) { // ECALL
              HartException ecallval = (HartException)(hs.privmode + 8);
              return create_exception(hs,ecallval, hs.pc);
            }
            if (hs.inst == 0b000000000001'00000'000'00000'1110011) { // EBREAK
              /*
              dbg_print("EBREAK called at ");
              dbg_print(hs.pc);
              dbg_endl();
              dump_state(hs);
              */
              return create_exception(hs,HartException::BPOINT, hs.pc);
            }
            
            if (hs.inst == 0b0011000'00010'00000'000'00000'1110011) { // MRET
              hs.mstatus = (hs.mstatus & ~(0b1 << 3)) | ((hs.mstatus & (0b1 << 7)) >> (7-3)); // move mstatus.MPIE to mstatus.MIE
              hs.mstatus |= (0b1 << 7); // set mstatus.MPIE
              
              hs.pc = hs.mepc & ~(0b11); // mepc
              pc_chgd = true;
              
              hs.privmode = (hs.mstatus & (0b11 << 11)) >> 11; // mstatus.MPP
              hs.mstatus &= ~(0b11 << 11); // set mstatus.MPP to 0b00 (U-mode)
              if (hs.privmode != 0b11) hs.mstatus &= ~(0b1 <<  17); // clear mstatus.MPRV
              
              hs.chk_int = true;
              break;
            }
            
            if (hs.inst == 0b0001000'00010'00000'000'00000'1110011) { // SRET
              if (hs.mstatus & (0b1 << 22)) { // TSR bit
                return create_exception(hs,HartException::ILLINST, hs.inst);
              }
              hs.mstatus = (hs.mstatus & ~(0b1 << 1)) | ((hs.mstatus & (0b1 << 5)) >> (5-1)); // move mstatus.SPIE to mstatus.SIE
              hs.mstatus |= (0b1 << 5); // set mstatus.SPIE
      
              hs.pc = hs.sepc & ~(0b11); // sepc
              pc_chgd = true;
              
              hs.privmode = (hs.mstatus & (0b1 << 8)) >> 8; // mstatus.SPP
              hs.mstatus &= ~(0b1 << 8); // set mstatus.SPP to 0b0 (U-mode)
              if (hs.privmode != 0b11) hs.mstatus &= ~(0b1 <<  17); // clear mstatus.MPRV
              
              hs.chk_int = true;
              break;
            }
            
            if (hs.inst == 0b0001000'00101'00000'000'00000'1110011) { // WFI
              if (hs.mstatus & (0b1 << 21)) { // TW bit
                return create_exception(hs,HartException::ILLINST, hs.inst);
              }
              
              std::this_thread::sleep_for(std::chrono::microseconds(100));
              hs.chk_int = true;
              break;
            }
            
            if (funct7 == 0b0001001) { // SFENCE.VMA
              if (hs.mstatus & (0b1 << 20)) { // TVM bit
                return create_exception(hs,HartException::ILLINST,hs.inst);
              }
              // always clears the whole TLB, which is a valid behaviour
              tlb_clear(&hs.tlb);
              break;
            }
            
            // CSR instructions
            imm &= 0xFFF; // remove the sign extended bits, if any
            if (((imm & (0b11 << 8)) >> 8) > hs.privmode) { // privilege mode insufficient
              return create_exception(hs,HartException::ILLINST, hs.inst);
            } else {
              /*
              if (chk_ill_csr(imm)) {
                return create_exception(hs, HartException::ILLINST, hs.inst);
              }
              */
              if (imm == 0xC01) {
                hs.regs[rd] = aclint_mtime_get();
                break;
                
                // spike doesn't support rdtime for some reason, it is disabled for debugging
                //return create_exception(hs, HartException::ILLINST, hs.inst);
                //hs.csr[0xC01] = aclint_mtime_get(); // update time CSR
              }
              
              
              if (chk_ro0_csr(imm)) {
                hs.regs[rd] = 0;
                break; // not supported, return read-only 0
              }
              
              if (imm == 0x306 || imm == 0x106 || imm == 0x320) { // mcounteren, scounteren and mcountinhibit
                hs.regs[rd] = 5; // only cycle and instret are available
                break;
              }
              
              uint64_t wvalue;
              if (funct3 & 0b100){
                wvalue = rs1;
              } else {
                wvalue = hs.regs[rs1];
              }
              
              if (0x3A0 <= imm && imm < 0x3B0) { // pmpcfgX CSRs require special handling
                if (!pc_chgd) {
                  hs.pc += hs.inst_len / 8;
                }
                hs.minstret++; // minstret CSR
                return pmpcfg_rw(hs, (uint16_t)imm, (uint64_t*)&hs.regs[rd], wvalue, uint8_t(funct3 & 0b11));
              }
              
              // sstatus is mirrored to mstatus
              bool sstatus = (imm == 0x100);
              if (sstatus) imm = 0x300;
              // sie and sip are mirrored to mie and mip
              bool sie = (imm == 0x104);
              bool sip = (imm == 0x144);
              if (sie) imm = 0x304;
              if (sip) imm = 0x344;
              
              // TVM bit
              if (imm == 0x180 && (hs.mstatus & (0b1 << 20) ) ) {
                return create_exception(hs,HartException::ILLINST, hs.inst);
              }
              
              // translate CSR address to CSR ptrs
              uint64_t* csr_addr = csr_from_addr(hs, imm);
              if (!csr_addr) {
                //csr_addr = &hs.csr[imm];
                return create_exception(hs,HartException::ILLINST, hs.inst);
              }
              
              if (rd != 0){
                hs.regs[rd] = *csr_addr;
              }
              
              if (0x3B0 <= imm && imm < (0x3B0 + PMP_COUNT)) { // pmpaddrX
                if (hs.pmp_lockedaddr[imm - 0x3B0]) { // locked
                  break;
                }
              }
              
              if ((funct3 & 0b11) == 0b01 || (wvalue && rs1)){ // a write is needed
                if (((imm & (0b11 << 10)) == (0b11 << 10)) && !(sstatus || sie || sip) ){
                  return create_exception(hs,HartException::ILLINST, hs.inst); // writing read-only CSR
                }
                // TODO: allow dynamically configuring extensions
                if (imm == 0x301) break; // misa writes are currently not supported
                
                if (imm == 0x300 || imm == 0x302 || imm == 0x303 || imm == 0x304 || imm == 0x344) { // interrupt related CSRs, we must check for interrupts
                  hs.chk_int = true;
                }
                
                switch (funct3 & 0b11){
                  case 0b01:
                    if (sip) wvalue &= ~(0b100010001000); // block writes to machine mode interrupts
                    *csr_addr = wvalue;
                    break;
                  case 0b10:
                    if (sip) wvalue &= ~(0b100010001000); // block writes to machine mode interrupts
                    if (wvalue) *csr_addr |= wvalue;
                    break;
                  case 0b11:
                    if (sip) wvalue |= (0b100010001000); // block writes to machine mode interrupts
                    if (wvalue) *csr_addr &= ~wvalue;
                    break;
                }
              }
              if (imm == 0x300) {
                hs.mstatus &= ~(0b10101);
                
                // force SXL and UXL to be 64-bits
                hs.mstatus &= ~(0b1111LL << 32);
                hs.mstatus |= (0b1010LL << 32);
              }
              if (sstatus) {
                hs.sstatus = hs.mstatus; // mirror mstatus to sstatus
                hs.sstatus &= ~(0b11LL << 34); // remove SXL
              }
              if (sie) hs.sie = hs.mie & ~(0b100010001000); // sie mirroring mie, masking machine mode software, timer, external interrupts
              //if (sip) hs.sip = hs.mip;
            }
            break;
          
          
          // A extension
          // misaligned atomics are treated equally, so Zam is also implemented
          case inst_op_32::AMO:
            {
              bool amo_w = funct3 == 0b010; // true if 32 bit, false if 64 bit
              // lock guard locks the mutex at construction, and unlocks automatically at deconstruction at the end of its scope (end of this block)
              std::lock_guard<std::mutex> amo_lock(atomic_op_mtx);
              uint16_t funct5 = funct7 >> 2;
              // aq and rl are currently unused due to the synchronous nature of the memory
              /*
              bool aq,rl;
              aq = funct7 & 0b10;
              rl = funct7 & 0b01;
              */
              if (funct5 == 0b00011){ // SC
                uint64_t store_addr = hs.regs[rs1];
                uint64_t sc_phy_addr = page_table_walk(hs, store_addr, true);
                HANDLE_MEM_ERROR(S, store_addr);
                if (reservations[hs.hartid] == sc_phy_addr){
                  hs.mem_status = true;
                  if (amo_w) {
                    virt_mem_store<uint32_t>(hs,store_addr,hs.regs[rs2]);
                  } else {
                    virt_mem_store<uint64_t>(hs,store_addr,hs.regs[rs2]);
                  }
                  HANDLE_MEM_ERROR(S, store_addr)
                  hs.regs[rd] = 0;
                } else {
                  hs.regs[rd] = 1;
                }
                reservations[hs.hartid] = 0; // clear reservations
                break;
              }
              // otherwise, we have to load from mem
              
              // src is read early in case rd == rs2, otherwise the load to rd would clobber src
              const int64_t src = amo_w ? (int32_t)hs.regs[rs2] : hs.regs[rs2];
              const uint64_t usrc = amo_w ? (uint32_t)hs.regs[rs2] : hs.regs[rs2];
              
          	  uint64_t op_addr = hs.regs[rs1];
          	  hs.mem_status = true;
              if (amo_w) {
                hs.regs[rd] = virt_mem_fetch<uint32_t>(hs,op_addr);
              } else {
                hs.regs[rd] = virt_mem_fetch<uint64_t>(hs,op_addr);
              }
              HANDLE_MEM_ERROR(S, op_addr) // although this is a load operation, AMOs only throw store faults
              if (amo_w) hs.regs[rd] = (int32_t)(hs.regs[rd]); // sign extend
              
              if (funct5 == 0b00010){ // lr
                hs.mem_status = true; // notify page_table_walk that we are reading, not executing
                uint64_t lr_phy_addr = page_table_walk(hs,hs.regs[rs1],false);
                HANDLE_MEM_ERROR(S, hs.regs[rs1]);
                for (uint16_t i = 0; i < MACH_HART_COUNT;i++) {
                  if (hs.hartid != i && reservations[i] == lr_phy_addr) {
                    reservations[i] = 0; // remove existing reservations on this memory address by other harts
                  }
                }
                reservations[hs.hartid] = lr_phy_addr;
                break;
              }
              
              int64_t ovalue = amo_w ? (int32_t)hs.regs[rd] : hs.regs[rd];
              uint64_t uovalue = amo_w ? (uint32_t)hs.regs[rd] : hs.regs[rd];
              switch (funct5){
                case 0b00001:
                  ovalue = src;
                  break;
                case 0b00000:
                  ovalue += src;
                  break;
                case 0b00100:
                  ovalue ^= src;
                  break;
                case 0b01100:
                  ovalue &= src;
                  break;
                case 0b01000:
                  ovalue |= src;
                  break;
                case 0b10000:
                  ovalue = (ovalue < src) ? ovalue : src;
                  break;
                case 0b10100:
                  ovalue = (ovalue > src) ? ovalue : src;
                  break;
                case 0b11000:
                  ovalue = (uovalue < usrc) ? uovalue : usrc;
                  break;
                case 0b11100:
                  ovalue = (uovalue > usrc) ? uovalue : usrc;
                  break;
              }
              
              hs.mem_status = false;
              if (amo_w) {
                virt_mem_store<uint32_t>(hs,op_addr,ovalue);
              } else {
                virt_mem_store<uint64_t>(hs,op_addr,ovalue);
              }
              HANDLE_MEM_ERROR(S, op_addr)
            }
            break;
          
          default:
            dbg_print("unknown instruction");
            dbg_endl();
            dump_state(hs);
      };
      break;
    default:
      dbg_print("unsupported instruction length execute:");
      dbg_print(hs.inst_len,false);
      dbg_endl();
      hs.inst_len = 0;
      return create_exception(hs,HartException::ILLINST, hs.inst);
  };
  
  if (!pc_chgd) {
    hs.pc += hs.inst_len / 8;
  }
  
  hs.minstret++; // minstret CSR
  return HartException::NOEXC;
}

/*
// returns true if the csr is illegal
bool chk_ill_csr(uint16_t csrno) {
  uint16_t csr_ident = csrno & 0xff;
  uint64_t csr_maj = csrno & 0xf00;
  if (csr_ident == 0x50 || csr_ident == 0x51 || (csr_ident == 0xB0 && ((csrno & 0xC00) == 0xC00) ) ) { // RISC-V AIA not supported
    return true;
  }
  
  if (0x0C <= csr_ident && csr_ident <= 0x0F && csr_maj != 0xB00) { // Smstateen
    return true;
  }
  
  if (csrno == 0xDA0) { // scountovf
    return true;
  }
  
  if (csrno == 0x14D) { // stimecmp
    return true;
  }
  
  if ( (csr_ident == 0x21 || csr_ident == 0x22) && (csr_maj == 0x300 || csr_maj == 0x700)) {
    return true;
  }
  
  return false;
}
*/

bool chk_ro0_csr(uint16_t csrno) {
  /*
  uint16_t csr_ident = csrno & 0xff;
  uint64_t csr_maj = csrno & 0xf00;
  */
  if ((0xB03 <= csrno && csrno <= 0xB1F) || (0x323 <= csrno && csrno <= 0x33F)) {
    return true;
  }
  
  if (csrno == 0x30A) { // menvcfg
    return true;
  }
  
  return false;
}

void dump_state(HartState &hs){
  dbg_print("hartid:");
  dbg_print(hs.hartid);
  dbg_print(" pc:");
  dbg_print(hs.pc);
  dbg_print(" inst:");
  dbg_print(hs.inst);
  dbg_endl();
  
  for (size_t i = 0; i<32 ; i++){
    dbg_print("x");
    dbg_print(i,false);
    dbg_print(":");
    dbg_print(hs.regs[i]);
    if ((i&1)==1) { // odd numbers
      dbg_endl(); // 2 regs per line
    } else {
      dbg_print(" ");
    }
  }
}

void reset_state(HartState &hs, uint16_t hartid){
  memset(&hs, 0, sizeof(hs));
  hs.hartid = hartid;
  hs.privmode = 0b11; // M mode
  
  hs.misa = MISA;
  
  hs.mstatus = MSTATUS; // mstatus
  hs.sstatus = hs.mstatus; // mirror mstatus to sstatus
  
  hs.hartid = hartid;
  
  hs.mie = 0b101010101010; // mie enable all standard interrupts
  hs.sie = hs.mie & ~(0b100010001000); // sie mirroring mie, masking machine mode software, timer, external interrupts
  
  hs.satp = 0x0; // set satp to Bare
  
  // set 0th pmp entry to allow all accesses
  hs.pmpaddr[0] = 0x003fffffffffffff;
  hs.pmpcfg[0] |= 0b00011111ULL;
  
  // legacy
  /*
  hs.csr[0xF11] = 0; // mvendorid
  hs.csr[0xF12] = 0; // marchid
  hs.csr[0xF13] = 0; // mimpid
  hs.csr[0xF14] = hartid;
  
  hs.csr[0x301] = MISA;
  hs.csr[0x300] = MSTATUS;
  hs.csr[0x100] = hs.csr[0x300];
  
  hs.csr[0x304] = 0b101010101010; // mie enable all standard interrupts
  
  hs.csr[0x104] = hs.csr[0x304] & ~(0b100010001000); // sie mirroring mie, masking machine mode software, timer, external interrupts
  
  // TESTING:
  // set 0th pmp entry to allow all accesses
  hs.csr[0x3B0] = 0x003fffffffffffff;
  hs.csr[0x3A0] |= 0b00011111ULL;
  
  hs.csr[0x180] = 0x0; // set satp to Bare
  
  hs.csr[0x320] = 3; // mcountinhibit only enable instret, time, cycle
  */
}

uint64_t mvendorid = 0;
uint64_t marchid = 0;
uint64_t mimpid = 0;

uint64_t* csr_from_addr(HartState &hs, uint16_t addr) {
  switch (addr) {
    // M mode CSRs
    case 0x300:
      return &hs.mstatus;
    case 0x301:
      return &hs.misa;
    case 0x302:
      return &hs.medeleg;
    case 0x303:
      return &hs.mideleg;
    case 0x304:
      return &hs.mie;
    case 0x305:
      return &hs.mtvec;
    case 0x340:
      return &hs.mscratch;
    case 0x341:
      return &hs.mepc;
    case 0x342:
      return &hs.mcause;
    case 0x343:
      return &hs.mtval;
    case 0x344:
      return &hs.mip;
    
    // S mode CSRs
    case 0x100:
      return &hs.sstatus;
    case 0x104:
      return &hs.sie;
    case 0x105:
      return &hs.stvec;
    case 0x140:
      return &hs.sscratch;
    case 0x141:
      return &hs.sepc;
    case 0x142:
      return &hs.scause;
    case 0x143:
      return &hs.stval;
    /*
    case 0x144:
      return &hs.sip;
    */
    case 0x180:
      return &hs.satp;
    
    // machine information CSRs
    case 0xF11:
      return &mvendorid;
    case 0xF12:
      return &marchid;
    case 0xF13:
      return &mimpid;
    case 0xF14:
      return &hs.mhartid;
  }
  
  if (0x3B0 <= addr && addr < (0x3B0 + PMP_COUNT)) { // pmpaddrX
    return &hs.pmpaddr[addr - 0x3B0];
  }
  
  // unknown CSR
  return nullptr;
}

HartException pmpcfg_rw(HartState &hs, uint16_t addr, uint64_t* rvalue, uint64_t wvalue, uint8_t funct) {
 if (addr & 0b1) { // odd-numbered CSR, illegal under RV64
   return create_exception(hs,HartException::ILLINST,hs.inst);
 }
 
 uint8_t start_index = 8/2 * (addr - 0x3A0); // the CSRs come in multiples of two, and each CSR holds 8 pmpcfg
 
 uint64_t readval = 0;
 uint8_t writeval = 0;
 for (uint16_t i = 0; i < 8; i++) {
   uint16_t cfgindex = start_index + i;
   readval |= hs.pmpcfg[cfgindex] << (8 * i);
   writeval = (wvalue >> (8 * i)) & 0xFF;
   // check lock bits
   if ( (hs.pmpcfg[cfgindex] & (0b1 << 7)) ) {
     continue; // this cfg is locked
   }
   switch (funct) {
     case 0b01:
       hs.pmpcfg[cfgindex] = writeval;
       break;
     case 0b10:
       hs.pmpcfg[cfgindex] |= writeval;
       break;
     case 0b11:
       hs.pmpcfg[cfgindex] &= ~writeval;
       break;
   }
   if (hs.pmpcfg[cfgindex] & (0b1 << 7)) {
     hs.pmp_lockedaddr[cfgindex] = true; // this cfg was just locked, lock the corresponding addr too
     if ((hs.pmpcfg[cfgindex] & 0b11000) == 0b01000 && cfgindex != 0) { // we locked a TOR entry, lock the previous addr too
       hs.pmp_lockedaddr[cfgindex - 1] = true;
     }
   }
 }
 *rvalue = readval;
 sync_exp_pmp(hs); // sync the new PMP configuration to the expanded PMP entries
 return HartException::NOEXC;
}
