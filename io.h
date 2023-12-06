#pragma once
#include <cstdint>

void io_init(bool skip_pty = false);
void io_uninit();

// debug print functions

void dbg_print(const char* msg);
void dbg_print(int64_t num, bool hex = true);
void dbg_endl();

void dbgerr_print(const char* msg);
void dbgerr_print(int64_t num, bool hex = true);
void dbgerr_endl();

char dbg_getc();

// pseudoterminal functions for emulated terminals

void pty_init(bool skip = false);
void pty_uninit();

void pty_print(const char* msg);
char pty_getc();
void pty_endl();
