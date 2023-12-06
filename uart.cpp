// NS16550A-compatible UART serial terminal

#include "uart.h"

#include <cstdint>
#include <cstring>
#include <thread>
#include <mutex>
#include <condition_variable>

#include "io.h"
#include "plic.h"

#define IBUF_SIZE 256
// circular buffer for input
char input_buffer[IBUF_SIZE];
char* read_ptr;
char* write_ptr;

uint64_t output_byte;
bool dlab = false;
bool rready = false;
uint8_t ls = 0;
uint8_t ms = 0;
uint8_t fifo_trigger_lvl = 1;

uint8_t regs[8] = {0};

std::thread uart_thread;
std::mutex uart_mtx;
std::condition_variable uart_cv;
bool tx_required = false;
bool uart_end = false;

char output[IBUF_SIZE];
size_t tx_offset = 0;

void uart_init() {
  read_ptr = input_buffer;
  write_ptr = input_buffer;
  regs[2] = 0b01;
  regs[5] = 0b01100000;
  uart_thread = std::thread(uart_loop);
}

void uart_loop() {
  std::unique_lock<std::mutex> uart_lock (uart_mtx);
  while (true) {
    uart_cv.wait(uart_lock, []{return tx_required || uart_end;});
    if (uart_end) break;
    if (output[tx_offset] == 0xa) { // last character is a line feed
      output[tx_offset] = 0;
      pty_print(output);
      pty_endl();
    } else {
      pty_print(output);
    }
    tx_offset = 0;
    memset(output, 0, IBUF_SIZE);
    tx_required = false;
    regs[5] |= 0b1100000;
    if (regs[1] & 0b10) { // THR empty interrupt enabled
      if (regs[2] != 0b0100) { // receive interrupts have higher priority, don't overwrite
        regs[2] = 0b0010;
      }
      plic_send_int(10);
    }
  }
}

void uart_uninit() {
  uart_end = true;
  uart_cv.notify_all();
  uart_thread.join();
}

void* uart_r(uint64_t offset, [[maybe_unused]] uint8_t len) {
  switch(offset) {
    case 0:
      if (dlab) {
        output_byte = ls;
      } else {
        if (rready) {
          output_byte = *read_ptr++;
          if ((read_ptr - input_buffer) > IBUF_SIZE) read_ptr = input_buffer;
        } else {
          output_byte = 0;
        }
      }
      break;
    case 1:
      if (dlab) {
        output_byte = regs[1];
      } else {
        output_byte = ms;
      }
      break;
    default:
      output_byte = regs[offset];
  }
  
  if (offset == 2 && regs[2] == 0b0010) {
    regs[2] = 0b0001;
  }
  
  rready = read_ptr < write_ptr;
  if (rready) {regs[5] |= rready;}
  else {regs[5] &= (~1);}
  
  return &output_byte;
}

void uart_w(uint64_t offset, void* dataptr, [[maybe_unused]] uint8_t len) {
  regs[offset] = *(uint8_t*)dataptr;
  if (offset == 3) {
    dlab = regs[3] & (0b1 << 7);
  }
  
  if (offset == 0) {
    if (regs[2] == 0b0010) { // THR empty
      regs[2] = 0b0001;
    }
    while (tx_required); // spinlock to prevent race condition on output
    char data_char = *(char*)dataptr;
    output[tx_offset] = data_char;
    tx_offset++;
    //regs[5] &= ~(0b1100000);
    /* if (tx_offset >= (IBUF_SIZE - 4) || data_char == 0xa) */ {
      std::lock_guard<std::mutex> uart_lock(uart_mtx);
      tx_required = true;
      uart_cv.notify_one();
    }
  }
  
  if (offset == 2) { // FCR
    switch (regs[2] >> 6) {
      case 0:
        fifo_trigger_lvl = 1;
        break;
      case 1:
        fifo_trigger_lvl = 4;
        break;
      case 2:
        fifo_trigger_lvl = 8;
        break;
      case 3:
        fifo_trigger_lvl = 14;
        break;
    }
  }
}

// 0 for non-existent interrupt
const uint8_t uart_int_prio[] = {4, 3, 2, 1, 0, 6, 2, 5};

void uart_sendint(uint8_t code) {
  if (uart_int_prio[code] < uart_int_prio[regs[]]) {
    return; // higher-priority interrupt is pending
  }
}

// takes in one character from console and monitors interrupts
void uart_chk() {
  char ch = pty_getc();
  if (ch == -1) return;
  *write_ptr = ch;
  write_ptr++;
  if ((write_ptr - read_ptr) > fifo_trigger_lvl) {
    if (regs[1] & 1) { // data ready interrupt active
      regs[2] = 0b0100;
      plic_send_int(10);
    }
  }
  if ((write_ptr - input_buffer) > IBUF_SIZE) write_ptr = input_buffer;
}
