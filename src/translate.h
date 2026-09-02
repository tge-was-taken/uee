#pragma once

#include "hook.h"
#include "elf.h"
#include "translator.h"
#include "interpreter.h"
#include "memory.h"
#include "ee_context.h"
#include "types.h"

#include <span>

#include <string>
#include <string_view>

namespace uee {

// Every translated unit includes this, and kernel.h reaches spdlog.
class Kernel;

struct Machine;
using FunctionRunner = bool (*)(Machine& machine, u32 pc);

struct Machine {
    Interpreter& interpreter;
    Memory& memory;
    Kernel& kernel;
    FunctionRunner functions = nullptr;
    std::span<Hook* const> hooks = {};
    StopRequest* stopRequest = nullptr;

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

// What a block of emitted code ran. The clock, the interrupt handlers and the
// other threads all depend on being told.
void countExecuted(Machine& machine, u32 instructions);

TranslatedSource translate(Memory& memory, const Program& program, usize units,
                           std::string_view declarationsHeader);

}  // namespace uee
