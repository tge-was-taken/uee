#pragma once

#include "fpu.h"
#include "opcodes.h"

namespace uee {

struct Instruction;

// false if this backend does not model the op
bool executeFpu(Fpu& fpu, Op op, Instruction insn);

}  // namespace uee
