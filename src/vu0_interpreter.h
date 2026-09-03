#pragma once

#include "memory.h"
#include "opcodes.h"
#include "vu.h"

namespace uee {

struct Instruction;

// false if this backend does not model the op
bool executeVu0(Vu& vu0, Memory& memory, Op op, Instruction insn);

}  // namespace uee
