#pragma once

#include "emotion_engine.h"
#include "fpu_interpreter.h"
#include "instruction.h"
#include "memory.h"
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
        : mMemory(memory), mFpu(machine.fpu), mVu0(machine.vu0) {}

    Status execute(Instruction insn);

    bool hasDelaySlot() const { return mDelaySlot; }

    EmotionEngine machine;

private:
    Memory& mMemory;
    FpuInterpreter mFpu;
    Vu0Interpreter mVu0;
    bool mDelaySlot = false;
};

}  // namespace uee
