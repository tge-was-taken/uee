#pragma once

#include "elf.h"
#include "iop_context.h"
#include "iop_interpreter.h"
#include "iop_kernel.h"
#include "memory.h"
#include "types.h"

#include <string>

namespace uee {

struct IopMachine;
using IopFunctionRunner = bool (*)(IopMachine& machine, u32 pc);

struct IopMachine {
    IopInterpreter& interpreter;
    Memory& memory;
    IopKernel& kernel;
    IopFunctionRunner functions = nullptr;

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

std::string translateIop(Memory& memory, const Program& program);

}  // namespace uee
