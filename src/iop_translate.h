#pragma once

#include "elf.h"
#include "translator.h"
#include "iop_context.h"
#include "iop_interpreter.h"
#include "iop_kernel.h"
#include "memory.h"
#include "types.h"

#include <set>
#include <span>
#include <string>
#include <string_view>

namespace uee {

struct IopMachine;
class IopScheduler;
using IopFunctionRunner = bool (*)(IopMachine& machine, u32 pc);

// Named by the module, not by a position the load order decides.
struct TranslatedImport {
    std::string_view module;
    u32 id;
};

// Translated ahead of time, and only correct between base and end.
struct TranslatedIrx {
    std::string_view name;
    u32 base;
    u32 end;
    std::span<const TranslatedImport> imports;
    IopFunctionRunner run;
    // Where the loader put this module's block in the kernel's table.
    u32 importBase = 0;
};

struct IopMachine {
    IopInterpreter& interpreter;
    Memory& memory;
    IopKernel& kernel;
    std::span<TranslatedIrx* const> modules;
    // One entry per page of the region the modules are packed into, so finding
    // the module a pc belongs to is a load rather than a walk over all of them.
    std::span<TranslatedIrx* const> modulePages;
    // What the run charges against its budget. Emitted code reports the
    // instructions it ran, the way the interpreter counts each one it steps.
    u64* executed = nullptr;
    const u64* limit = nullptr;
    IopScheduler* scheduler = nullptr;

    IopContext& iop() const { return interpreter.iop; }
};

inline void countIopOnly(IopMachine& machine, u32 instructions) {
    if (machine.executed != nullptr) {
        *machine.executed += instructions;
    }
}

// Charging only what runs keeps a block bigger than the budget from being
// charged forever and never run.
inline bool countIop(IopMachine& machine, u32 instructions) {
    if (machine.limit != nullptr && *machine.executed >= *machine.limit) {
        return false;
    }
    countIopOnly(machine, instructions);
    return true;
}

bool stopAtIop(IopMachine& machine, u32 blockStart);
bool hostService(IopMachine& machine, u32 service, u32 pc);

inline bool leave(IopContext& iop, u32 pc, u32 returnTo) {
    iop.cpu.pc = pc;
    return pc == returnTo;
}

void interpreterFallback(IopMachine& machine, u32 pc);

void interpreterFallbackDelaySlot(IopMachine& machine, u32 pc);

bool call(IopMachine& machine, u32 target, u32 returnTo);

// Answers an import stub, which returns to $ra rather than falling through.
// False leaves the pc on the stub.
bool hostCall(IopMachine& machine, u32 index, u32 pc);

bool runTranslated(IopMachine& machine, u32 pc);

// The loader matches on a file name, which carries an extension a C++ name
// cannot. Empty means they are the same.
TranslatedSource translateIop(Memory& memory, const Program& program, std::string_view name,
                              std::span<const Import> imports, usize units,
                              std::string_view declarationsHeader,
                              std::string_view loadName = {},
                              std::span<const u32> entries = {},
                              const std::set<std::string>& guestLibraries = {});

}  // namespace uee
