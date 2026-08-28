#pragma once

#include "fpu_interpreter.h"
#include "instruction.h"
#include "memory.h"
#include "ee_context.h"
#include "stop.h"
#include "vu0_interpreter.h"

namespace uee {

class Interpreter {
public:
    explicit Interpreter(Memory& memory) : mMemory(memory) {}

    Status execute(Instruction insn);

    bool hasDelaySlot() const { return mDelaySlot; }
    bool tookBranch() const { return mBranchTaken; }
    bool branchYieldsDelaySlot() const { return mBranchYields; }

    EeContext ee;
    // Not part of the context an interrupt saves: VU1 keeps running its own
    // microprogram across one.
    Vu vu1;

private:
    Memory& mMemory;
    bool mDelaySlot = false;
    bool mBranchYields = false;
    bool mBranchTaken = false;
};

}  // namespace uee
