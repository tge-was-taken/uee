#pragma once

#include "memory.h"
#include "opcodes.h"
#include "vu0.h"

namespace uee {

struct Instruction;

// Macro mode still reaches VU0 data memory, which the EE also sees through its
// own window, so both go through the one Memory rather than a private copy.
class Vu0Interpreter {
public:
    Vu0Interpreter(Vu0& state, Memory& memory) : mState(state), mMemory(memory) {}

    // false if this backend does not model the op
    bool execute(Op op, Instruction insn);

private:
    Vu0& mState;
    Memory& mMemory;
};

}  // namespace uee
