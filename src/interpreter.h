#pragma once

#include "fpu_interpreter.h"
#include "instruction.h"
#include "memory.h"
#include "ee_context.h"
#include "vu0_interpreter.h"

namespace uee {

enum class Status {
    Executed,
    Unimplemented,   // a real encoding this interpreter does not model yet
    Undecodable,     // no source names this encoding
};

class Interpreter {
public:
    explicit Interpreter(Memory& memory) : mMemory(memory) {}

    Status execute(Instruction insn);

    bool hasDelaySlot() const { return mDelaySlot; }
    bool tookBranch() const { return mBranchTaken; }

    EeContext ee;

private:
    Memory& mMemory;
    bool mDelaySlot = false;
    bool mBranchTaken = false;
};

}  // namespace uee
