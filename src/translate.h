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
    Vu& vu1() const { return interpreter.vu1; }
};

inline bool leave(EeContext& ee, u32 pc, u32 returnTo) {
    ee.cpu.pc = pc;
    return pc == returnTo;
}

void step(Machine& machine, u32 pc);

void stepDelaySlot(Machine& machine, u32 pc);

bool call(Machine& machine, u32 target, u32 returnTo);

bool syscall(Machine& machine, s32 number, u32 resumePc);

std::string translate(Memory& memory, const Program& program);

}  // namespace uee
