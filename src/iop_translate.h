#pragma once

#include "elf.h"
#include "translator.h"
#include "iop_context.h"
#include "iop_interpreter.h"
#include "iop_kernel.h"
#include "memory.h"
#include "types.h"

#include <span>
#include <string>
#include <string_view>

namespace uee {

struct IopMachine;
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

    IopContext& iop() const { return interpreter.iop; }
};

inline bool leave(IopContext& iop, u32 pc, u32 returnTo) {
    iop.cpu.pc = pc;
    return pc == returnTo;
}

void step(IopMachine& machine, u32 pc);

void stepDelaySlot(IopMachine& machine, u32 pc);

bool call(IopMachine& machine, u32 target, u32 returnTo);

// Answers an import stub, which returns to $ra rather than falling through.
// False leaves the pc on the stub.
bool hostCall(IopMachine& machine, u32 index, u32 pc);

// Runs pc if a translated module owns it.
bool runTranslated(IopMachine& machine, u32 pc);

TranslatedSource translateIop(Memory& memory, const Program& program, std::string_view name,
                              std::span<const Import> imports, usize units,
                              std::string_view declarationsHeader);

}  // namespace uee
