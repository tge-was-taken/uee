#pragma once

#include "opcodes.h"
#include "vu0.h"

namespace uee {

struct Instruction;

class Vu0Interpreter {
public:
    explicit Vu0Interpreter(Vu0& state) : mState(state) {}

    // false if this backend does not model the op
    bool execute(Op op, Instruction insn);

private:
    Vu0& mState;
};

}  // namespace uee
