#pragma once
#include <cstdint>

#include "cpu.h"

void aclint_mtimer_init();
void aclint_mtimer_reset();
void aclint_mswi_init();
void aclint_sswi_init();

uint64_t aclint_mtime_get();
void* aclint_mtimer_r (uint64_t offset, uint8_t len);
void aclint_mtimer_w (uint64_t offset, void* dataptr, uint8_t len);
void aclint_mtimer_chk(HartState& hs);

uint64_t readtime();

void* aclint_mswi_r (uint64_t offset, uint8_t len);
void aclint_mswi_w (uint64_t offset, void* dataptr, uint8_t len);
void aclint_mswi_chk(HartState& hs);
