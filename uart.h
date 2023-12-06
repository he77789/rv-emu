#pragma once
#include <cstdint>

void uart_init();
void uart_loop();
void uart_uninit();

void* uart_r(uint64_t offset, uint8_t len);
void uart_w(uint64_t offset, void* dataptr, uint8_t len);

void uart_sendint(uint8_t code);

void uart_chk();
