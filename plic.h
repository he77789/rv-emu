#pragma once

#include <cstdint>

#define PLIC_SOURCE_COUNT 16

void plic_init();

void plic_send_int(uint16_t source);
void plic_notify_ctx(uint16_t ctx);
void plic_notify_finish_ctx(uint16_t ctx);
void plic_try_int(uint16_t source);
void plic_try_int_all();
bool plic_find_en(uint16_t ctx, uint16_t source);

void plic_chk();

void* plic_r (uint64_t offset, uint8_t len);
void plic_w (uint64_t offset, void* dataptr, uint8_t len);
