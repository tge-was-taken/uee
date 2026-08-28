#pragma once

#include "memory.h"
#include "opcodes.h"
#include "vu.h"

namespace uee {

struct Instruction;

// Macro mode still reaches VU0 data memory, which the EE also sees through its
// own window, so both go through the one Memory rather than a private copy.
// false if this backend does not model the op
bool executeVu0(Vu& vu0, Memory& memory, Op op, Instruction insn);

}  // namespace uee
