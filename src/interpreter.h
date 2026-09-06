#pragma once

#include "fpu_interpreter.h"
#include "instruction.h"
#include "host_float.h"
#include "memory.h"
#include "ee_context.h"
#include "stop.h"
#include "vu0_interpreter.h"

namespace uee {

class Interpreter {
public:
    // Nothing runs a guest float without one of these, so this is where the
    // host unit is put into the mode host_float.h needs.
    explicit Interpreter(Memory& memory) : mMemory(memory) { host::configureHostFloat(); }

    Status execute(Instruction insn);

    bool hasDelaySlot() const { return mDelaySlot; }
    bool tookBranch() const { return mBranchTaken; }
    bool branchYieldsDelaySlot() const { return mBranchYields; }

    EeContext ee;

private:
    Memory& mMemory;
    bool mDelaySlot = false;
    bool mBranchYields = false;
    bool mBranchTaken = false;
};

}  // namespace uee
