#pragma once

#include "iop_context.h"
#include "iop_translate.h"
#include "irx.h"
#include "stop.h"
#include "types.h"

namespace uee {

constexpr u32 kIopModuleBase = 0x00010000;
constexpr u32 kIopStackTop = 0x001F0000;
constexpr u32 kIopStackSize = 0x00010000;
constexpr u32 kIopModuleReturn = 0x0FFFFFF0;
constexpr u32 kIopThreadReturn = 0x0FFFFFE0;

void enterModule(IopContext& iop, const IrxModule& module);

Stop run(IopMachine& machine, u64 instructionLimit);

}  // namespace uee
