#include <cstdio>
#include <csignal>
#include <unistd.h>
#include <cstring>
#include <thread>
#include <vector>

#include "cpu.h"
#include "mem.h"
#include "io.h"
#include "constants.h"
#include "hartexc.h"

#include "aclint.h"
#include "plic.h"
#include "uart.h"
#include "virtio_mmio_blk.h"

#define HELP_MSG \
"USAGE: ./main <args>\n\
Possible arguments:\n\
-f <path to firmware>\n\
-k <path to kernel>\n\
-i <path to initrd>\n\
-m <memory size, default 512MiB>\n\
-c <hart count, default 1>\n\
-d <path to device tree blob>\n\
-e dump the whole memory into a file named \"mem_dump\" at exit\n\
-p disable PTY setup for emulated UART terminal, and use stdio instead\n\
-h print this help message and exit\n\
-s enable signature dumping and specify signature file\
"

void sigint_handler(int signum);
void hart_init(HartState& hs, uint16_t hartid);
void hw_init();
void hw_perhart_update(HartState& hs);
void hw_update();
void hw_uninit();
void hart_loop(HartState& hs);

std::vector<std::thread> hart_threads;

bool hart_start = false;
bool interrupted = false;

int exit_signum = 0;
bool dump_mem_atexit = false;

bool skip_pty = false;

int main(int argc, char** argv){
  // parse args
  char* fwfile = nullptr;
  char* kernelfile = nullptr;
  char* dtbfile = nullptr;
  char* initrdfile = nullptr;
  char* signaturefile = nullptr;
  char copt;
  while ((copt = getopt(argc, argv, "f:k:i:m::c::d:s:eph")) != -1){
    switch (copt){
      case 'f':
        fwfile = optarg;
        dbg_print("fw file:");
        dbg_print(fwfile);
        dbg_endl();
        break;
      case 'k':
        kernelfile = optarg;
        dbg_print("kernel file:");
        dbg_print(kernelfile);
        dbg_endl();
        break;
      case 'i':
        initrdfile = optarg;
        dbg_print("initrd file:");
        dbg_print(initrdfile);
        dbg_endl();
        break;
      case 'm':
        MACH_MEM_SIZE = atol(optarg);
        break;
      case 'c':
        MACH_HART_COUNT = atoi(optarg);
        break;
      case 'd':
        dtbfile = optarg;
        dbg_print("dtb file:");
        dbg_print(dtbfile);
        dbg_endl();
        break;
      case 's':
        signaturefile = optarg;
        sig_mode = true;
        break;
      case 'e':
        dump_mem_atexit = true;
        break;
      case 'p':
        skip_pty = true;
        break;
      case 'h': // print help and exit
        dbg_print(HELP_MSG);
        dbg_endl();
        return 0;
      default:
        // getopt already gives an error message for us
        /*
        char error_msg[25] = {0}; // the error message below should be 24 characters long, plus 1 for \0
        sprintf(error_msg, "Unknown error message: %s", (char*)&copt);
        dbg_print(error_msg);
        dbg_endl();
        */
        return 1;
    }
  }
  if (! (fwfile || kernelfile)) {
    dbgerr_print("Neither fw or kernel passed in command line!");
    dbgerr_endl();
    return -1;
  }

  signal(SIGINT,sigint_handler);
  io_init(skip_pty);

  hartlist = new HartState[MACH_HART_COUNT];
  
  for (uint16_t i = 0; i < MACH_HART_COUNT;i++) {
    hart_init(hartlist[i],i);
    #ifdef MEM_TRACE
    dump_state(hartlist[i]);
    #endif
  }
  hw_init();
  
  if (fwfile == nullptr || strncmp(fwfile,"none",sizeof("none")) == 0){ // no extra firmware (e.g. OpenSBI)
    /*
    addi x8, x0, 1025
    slli x8, x8, 21
    jalr x0, x8, 0
    */
    //memcpy(main_mem,"\x13\x04\x10\x40\x13\x14\x54\x01\x67\x00\x04\x00",12);
    
    // load the kernel directly in place of the firmware
    FILE* kf = fopen(kernelfile,"rb");
    if (!kf) return 2;
    dbg_print("kernel size:");
    dbg_print(fread(main_mem,1,MACH_MEM_SIZE,kf));
    dbg_endl();
    fclose(kf);
  } else {
    FILE* bf = fopen(fwfile,"rb");
    if (!bf) return 1;
    dbg_print("firmware size:");
    dbg_print(fread(main_mem,1,0x200'0000,bf));
    dbg_endl();
    fclose(bf);
    
    FILE* kf = fopen(kernelfile,"rb");
    if (!kf) return 2;
    dbg_print("kernel size:");
    dbg_print(fread(main_mem+0x20'0000,1,MACH_MEM_SIZE-0x20'0000,kf));
    dbg_endl();
    fclose(kf);
  }
  
  if (dtbfile) { // dtb present
    FILE* df = fopen(dtbfile,"rb");
    if (!df) return 3;
    dbg_print("dtb size:");
    dbg_print(fread(dtb_buf,1,MAX_DTB_SIZE,df));
    dbg_endl();
    fclose(df);
  }
  
  if (initrdfile) { // initrd present
    FILE* initf = fopen(initrdfile,"rb");
    if (!initf) return 4;
    dbg_print("initrd size:");
    dbg_print(fread(main_mem+0x820'0000,1,0x800'0000,initf));
    dbg_endl();
    fclose(initf);
  }
  
  hart_start = false; // don't start execution until all threads have been created
  for (uint16_t i = 0; i < MACH_HART_COUNT; i++) {
    hart_threads.emplace_back(std::thread(hart_loop,std::ref(hartlist[i])) );
  }
  
  dbg_print("threads created, starting");
  dbg_endl();
  aclint_mtimer_reset();
  hart_start = true;
  
  while (!interrupted) {
    for (size_t i = 0; i < MACH_HART_COUNT; i++) {
      hw_perhart_update(hartlist[i]);
      //setup_pending_int(hartlist[i]);
      hartlist[i].chk_int = true;
    }
    hw_update();
    //std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::microseconds(5000)); // update the emulated hardware once per 5000 us at most
  }
  
  // wait for threads to exit
  for (auto& t : hart_threads) {
    t.join();
  }
  
  if (sig_mode) {
    FILE* sf = fopen(signaturefile,"wb");
    if (sf) {
      dbg_print("dumping signature");
      dbg_endl();
      fwrite(main_mem + 0xF0'0000, 1, 512, sf);
      fclose(sf);
    } else {
      dbg_print("unable to open signature file for writing");
      dbg_endl();
    }
  }
  
  if (dump_mem_atexit) dump_mem();
  hw_uninit();
  io_uninit();
  return exit_signum;
}

void hart_init(HartState& hs, uint16_t hartid) {
  reset_state(hs,hartid);
  hs.pc = 0x1000;
  hs.regs[2] = 0x8000'0000 + MACH_MEM_SIZE - 1; // set stack pointer to end of memory
  hs.regs[1] = 0x8100'0000; // somewhere probably empty so main will get a zero instruction if it returns
  hs.regs[10] = hartid; // hartid in a0
  hs.regs[11] = 0x1100; // dtb address in a1
}

void hw_init() {  
  mem_init();
  aclint_mtimer_init();
  aclint_mswi_init();
  plic_init();
  uart_init();
  virtio_mmio_blk_init();
}

void hw_perhart_update(HartState& hs) {
  aclint_mtimer_chk(hs);
  aclint_mswi_chk(hs);
}

void hw_update() {
  uart_chk();
  plic_chk();
}

void hw_uninit() {
  mem_free();
  virtio_mmio_blk_uninit();
  uart_uninit();
}

void sigint_handler(int signum){
  dbg_print("Interrupted, dumping registers and cleaning up");
  dbg_endl();
  interrupted = true; // notify hart threads to stop
  for (uint16_t i = 0;i < MACH_HART_COUNT; i++) {
    dump_state(hartlist[i]);
  }
  
  exit_signum = signum; // save signum
  
  // the main thread should do these stuff for us, as we notified the main thread with the interrupted flag
  /*
  if (dump_mem_atexit) dump_mem();
  hw_uninit();
  io_uninit();
  exit(signum);
  */
}

void hart_loop(HartState& hs) {
  while (!hart_start) std::this_thread::yield(); // wait for hart_start signal
  
  while (cycle(hs) && !interrupted) {
    //hw_update(hs);
    
    /*
    // partial HTIF functionality
    // refer to https://github.com/riscv-software-src/riscv-isa-sim/issues/364
    uint64_t command = *(uint64_t*)&main_mem[0x1000];
    if ( ((command >> 56) == 0x0) && (command & 1)) { // device 0, command 1, exit
      dbg_print("HTIF EXIT:");
      dbg_print((command >> 1) & 0x7fffffffffff); // 47:1 of command
      dbg_endl();
      interrupted = true; 
    }
    if ( ((command >> 48) == 0x101)) { // device 1, command 1, print
      dbg_print((char*)&command);
    }
    main_mem[0x1000] = 0;
    */
  }
  interrupted=true;
}
