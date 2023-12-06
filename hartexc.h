#pragma once
#include <cstdint>
#include "cpu.h"

// setup CSRs for the exception
HartException create_exception(HartState &hs, HartException he, uint64_t tval);

// check for pending interrupts, and if one exists, sets up the exception
// returns if there's an exception
bool setup_pending_int(HartState &hs);
