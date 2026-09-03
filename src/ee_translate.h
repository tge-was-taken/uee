#pragma once

#include "budget.h"
#include "call_hooks.h"
#include "elf.h"
#include "translator.h"
#include "interpreter.h"
#include "memory.h"
#include "symbols.h"
#include "ee_context.h"
#include "types.h"

#include <optional>
#include <set>
#include <utility>
#include <string>
#include <string_view>

namespace uee {

// Every translated unit includes this, and kernel.h reaches spdlog.
class Kernel;
class EeMmio;
class Deadline;

struct Machine;
using FunctionRunner = bool (*)(Machine& machine, u32 pc);

struct Machine {
    Interpreter& interpreter;
    Memory& memory;
    EeMmio& mmio;
    Kernel& kernel;
    Budget& budget;
    FunctionRunner functions = nullptr;
    // Guest functions a frontend answers for, named by address at run time
    // rather than by what they are made of.
    CallHooks* hooks = nullptr;
    StopRequest* stopRequest = nullptr;
    // What bounds the run in progress. Only the run loop reads it; emitted code
    // is told the answer through the budget.
    Deadline* deadline = nullptr;

    EeContext& ee() const { return interpreter.ee; }
    Vu& vu1() const { return interpreter.vu1; }
};

// The address is written on every path, since a resumed function knows no
// caller and a tail call answers for one that is not waiting.
inline bool leave(EeContext& ee, u32 pc, u32 returnTo) {
    ee.cpu.pc = pc;
    return pc == returnTo;
}

// Out of line because the check in front of it is what every prologue pays.
bool askFrontend(Machine& machine, u32 pc);

inline bool enterHook(Machine& machine, u32 pc) {
    if (machine.hooks == nullptr || !machine.hooks->isHooked(pc)) {
        return false;
    }
    return askFrontend(machine, pc);
}

void interpreterFallback(Machine& machine, u32 pc);

void interpreterFallbackDelaySlot(Machine& machine, u32 pc);

bool call(Machine& machine, u32 target, u32 returnTo);

bool syscall(Machine& machine, s32 number, u32 resumePc);

// Reached by a fifth of blocks, so the nothing-due case is worth inlining.
bool service(Machine& machine, u32 instructions);

inline bool countExecuted(Machine& machine, u32 instructions) {
    Budget& budget = machine.budget;
    if (budget.stopped) {
        return false;
    }
    budget.executed += instructions;
    if (budget.executed < budget.nextService) {
        return true;
    }
    return service(machine, instructions);
}

// What a block owes the clock when nothing can ask it part way through: the
// count still has to be right, but no one is waiting on the answer.
inline void countOnly(Machine& machine, u32 instructions) {
    machine.budget.executed += instructions;
}

// Where the run stopped. Out of line and off the path a block normally takes,
// because every block has one of these and none of them expect to reach it.
bool stopAt(Machine& machine, u32 blockStart);

// The symbols are what put a guest function's own name on the code generated
// from it; without them the address alone names it.
TranslatedSource translate(Memory& memory, const Program& program, usize units,
                           std::string_view declarationsHeader,
                           const Symbols* symbols = nullptr,
                           const std::set<u32>& hookable = {},
                           std::optional<u32> gp = {},
                           const std::vector<std::pair<u32, u32>>& readOnly = {});

}  // namespace uee
