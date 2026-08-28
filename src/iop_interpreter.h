#pragma once

#include "instruction.h"
#include "iop_context.h"
#include "memory.h"
#include "stop.h"

namespace uee {

class IopInterpreter {
public:
    explicit IopInterpreter(Memory& memory) : mMemory(memory) {}

    Status execute(Instruction insn);

    bool hasDelaySlot() const { return mDelaySlot; }
    bool tookBranch() const { return mBranchTaken; }

    IopContext iop;

private:
    Memory& mMemory;
    bool mDelaySlot = false;
    bool mBranchTaken = false;
};

}  // namespace uee
