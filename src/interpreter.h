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
    explicit Interpreter(Memory& memory)
        : mMemory(memory), mFpu(ee.fpu), mVu0(ee.vu0) {}

    Status execute(Instruction insn);

    bool hasDelaySlot() const { return mDelaySlot; }

    EeContext ee;

private:
    Memory& mMemory;
    FpuInterpreter mFpu;
    Vu0Interpreter mVu0;
    bool mDelaySlot = false;
};

}  // namespace uee
