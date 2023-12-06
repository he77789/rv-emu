#include "hartexc.h"
#include "cpu.h"

// returns the exception it created, if it's not masked
// if the exception is masked, returns NOEXC
HartException create_exception(HartState &hs, HartException he, uint64_t tval){
  bool interrupt = (uint64_t)he & (0b1LL << 63);
  uint64_t cause = (uint64_t)he & ~(0b1LL << 63);
  uint64_t int_mask = 0b1LL << cause;
  
  if (interrupt 
      && (hs.privmode == 0b11) /*M mode*/
      && !(hs.mstatus & (0b1 << 3)) /* mstatus.MIE is 0*/){
    // interrupt for the current mode is disabled, do nothing
    goto disabled;
  }
  if (interrupt 
      && (hs.privmode == 0b01) /*S mode*/
      && !(hs.mstatus & (0b1 << 1)) /* mstatus.SIE is 0*/
      && (cause != 3) && (cause != 7) && (cause != 11) /* we can't disable M-mode interrupts in S-mode */){
    // interrupt for the current mode is disabled, do nothing
    goto disabled;
  }
  
  if ((interrupt ? hs.mideleg : hs.medeleg) & int_mask // medeleg, mideleg
      && hs.privmode <= 0x1 // we shouldn't delegate to S-mode if we are not in S- or U-mode
      ){
    // delegated to S-mode
    if (interrupt) {
      if (!(hs.sie & int_mask)) goto disabled; // sie
      // otherwise
      //hs.sip &= ~int_mask; // clear the corresponding bit in sip, if set
    }
    hs.mstatus = (hs.mstatus & ~(0b11 << 8)) | (hs.privmode << 8); // mstatus.SPP
    hs.privmode = 0b01; // S-mode

    hs.mstatus = (hs.mstatus & ~(0b1 << 5)) | ((hs.mstatus & (0b1 << 1)) << (5-1)); // move mstatus.SIE to mstatus.SPIE
    hs.mstatus &= ~(0b1 << 1); // clear mstatus.SIE

    hs.scause = (uint64_t)he; // scause
    //hs.sepc = hs.pc + (hs.inst_len / 8) * interrupt; // sepc, set to pc+4 if interrupt, and pc if trap
    hs.sepc = hs.pc;

    hs.stval = tval;
    
    hs.pc = hs.stvec & (~0b11); // stvec base
    if ((hs.stvec & 0b11) == 1){ // stvec vectored mode
      hs.pc += 4 * cause;
    }
    
  } else {
    // M-mode
    if (interrupt) {
      if (!(hs.mie & int_mask)) goto disabled; // mie
      // otherwise
      //hs.mip &= ~int_mask; // clear the corresponding bit in mip, if set
    }
    hs.mstatus = (hs.mstatus & ~(0b11 << 11)) | (hs.privmode << 11); // mstatus.MPP
    hs.privmode = 0b11; // M-mode
    
    hs.mstatus = (hs.mstatus & ~(0b1 << 7)) | ((hs.mstatus & (0b1 << 3)) << (7-3)); // move mstatus.MIE to mstatus.MPIE
    hs.mstatus &= ~(0b1 << 3); // clear mstatus.MIE
    
    hs.mcause = (uint64_t)he; // mcause
    //hs.mepc = hs.pc + (hs.inst_len / 8) * interrupt; // mepc, set to pc+4 if interrupt, and pc if trap
    hs.mepc = hs.pc;
    
    hs.mtval = tval;
    
    hs.pc = hs.mtvec & (~0b11); // mtvec base
    if ((hs.mtvec & 0b11) == 1){ // mtvec vectored mode
      hs.pc += 4 * cause;
    }
  }
  return he;
disabled:
  return HartException::NOEXC;
}

#define TRY_SETINT(x) if (create_exception(hs,x,0) != HartException::NOEXC) { return true; }

bool setup_pending_int(HartState &hs) {
  if ((hs.mstatus & 0b1000 || hs.privmode < 0b11) && hs.mip) { // handle mip
    if (hs.mip & (1 << 11)) { // MEIP
      TRY_SETINT(HartException::MEXTINT);
    }
    if (hs.mip & (1 << 3)) { // MSIP
      TRY_SETINT(HartException::MSOFTINT);
    }
    if (hs.mip & (1 << 7)) { // MTIP
      TRY_SETINT(HartException::MTIMEINT);
    }
    if (hs.mip & (1 << 9)) { // SEIP
      TRY_SETINT(HartException::SEXTINT);
    }
    if (hs.mip & (1 << 1)) { // SSIP
      TRY_SETINT(HartException::SSOFTINT);
    }
    if (hs.mip & (1 << 5)) { // STIP
      TRY_SETINT(HartException::STIMEINT);
    }
  }
  
  if ((hs.mstatus & 0b10) && hs.mip) { // handle sip
    if (hs.mip & (1 << 9)) { // SEIP
      TRY_SETINT(HartException::SEXTINT);
    }
    if (hs.mip & (1 << 1)) { // SSIP
      TRY_SETINT(HartException::SSOFTINT);
    }
    if (hs.mip & (1 << 5)) { // STIP
      TRY_SETINT(HartException::STIMEINT);
    }
  }
  
  return false;
}
