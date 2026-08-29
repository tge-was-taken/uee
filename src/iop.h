#pragma once

#include "iop_interpreter.h"
#include "iop_kernel.h"
#include "iop_run.h"
#include "irx.h"
#include "memory.h"
#include "symbols.h"
#include "types.h"

#include <span>
#include <string>
#include <vector>

namespace uee {

// Modules live from here up to the heap sysmem hands out.
constexpr u32 kIopHeapBase = 0x00100000;

// The IOP as the EE drives it: its code runs while a call from the EE is
// outstanding, not on a clock of its own.
class Iop {
public:
    explicit Iop(std::string& console);

    bool loadModule(const std::string& name, std::span<const u8> image);
    u32 callFunction(u32 address, u32 gp, std::span<const u32> arguments);

    const std::vector<IopRpcServer>& rpcServers() const { return mKernel.rpcServers(); }

    u32 allocateMemory(u32 size) { return mKernel.allocateMemory(size); }
    bool freeMemory(u32 address) { return mKernel.freeMemory(address); }
    void tickVblank();
    void reportThreads() const { mKernel.reportThreads(mSymbols); }

    Symbols& symbols() { return mSymbols; }

    Memory& memory() { return mMemory; }

private:
    Stop enter();
    bool settle();

    Memory mMemory = makeIopMemory();
    IopInterpreter mInterpreter{mMemory};
    std::vector<Import> mImports;
    IopKernel mKernel;
    Symbols mSymbols;
    std::vector<IrxModule> mLoaded;
    u32 mNextBase = kIopModuleBase;
};

}  // namespace uee
