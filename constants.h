#pragma once
#include <cstdint>

#include "cpu.h"

#define MAX_DTB_SIZE 32768

inline uint64_t MACH_MEM_SIZE = 0x2000'0000; // 512 MiB
inline uint16_t MACH_HART_COUNT = 1;

inline HartState* hartlist;

// trace memory accesses
//#define MEM_TRACE
// trace JALRs
//#define CALL_TRACE
// trace all PC
//#define PC_TRACE

// uncomment if PMP is not needed, gives a performance boost
//#define DISABLE_PMP

// slow down ACLINT MTIMER clock to increment once every cycle, instead of real-time
//#define SLOW_MTIMER

// allow misaligned loads and stores
#define ALLOW_MISALIGN

// RV64IMAC with M,S,U mode
#define MISA (0b10LL << 62) + 0b00000101000001000100000101LL

#define MSTATUS 0b00000000'00000000'00000000'00001010'00000000'00101100'00001000'10101010LL

// a place for pointers to point to that would always give 0
inline uint64_t ZERO = 0;

const uint64_t UINT64_ALL_ONES = -1;
