#pragma once

#include "elf.h"
#include "interpreter.h"
#include "kernel.h"
#include "memory.h"
#include "ee_context.h"
#include "types.h"

#include <string>

namespace uee {

struct Machine;
using FunctionRunner = bool (*)(Machine& machine, u32 pc);

struct Machine {
    Interpreter& interpreter;
    Memory& memory;
    Kernel& kernel;
    FunctionRunner functions = nullptr;

    EeContext& ee() const { return interpreter.ee; }
};

inline bool leave(EeContext& ee, u32 pc, u32 returnTo) {
    ee.cpu.pc = pc;
    return pc == returnTo;
}

// Comes back with the pc already past the instruction, so emitted code can
// carry straight on.
void step(Machine& machine, u32 pc);

// Runs one instruction as the delay slot of a transfer already resolved into
// pc. Emitted code hands the slot over whenever the slot can transfer control
// itself, so both backends resolve that the same way.
void stepDelaySlot(Machine& machine, u32 pc);

bool call(Machine& machine, u32 target, u32 returnTo);

bool syscall(Machine& machine, s32 number, u32 resumePc);

std::string translate(Memory& memory, const Program& program);

}  // namespace uee
