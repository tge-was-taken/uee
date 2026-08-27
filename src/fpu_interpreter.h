#pragma once

#include "fpu.h"
#include "opcodes.h"

namespace uee {

struct Instruction;

class FpuInterpreter {
public:
    explicit FpuInterpreter(Fpu& state) : mState(state) {}

    // false if this backend does not model the op
    bool execute(Op op, Instruction insn);

private:
    Fpu& mState;
};

}  // namespace uee
