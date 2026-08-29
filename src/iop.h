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

// The IOP as the EE drives it: modules load into its RAM and its code runs
// while a call from the EE is outstanding, not on a clock of its own.
// Whether this runtime answers for a module or the game's own copy runs.
enum class ModuleSource { Host, Guest };

class Iop {
public:
    // Translated modules keep their bases; everything else loads above.
    Iop(std::string& console, std::span<TranslatedIrx* const> translated = {},
        std::span<const std::string> hostModules = {});

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

    TranslatedIrx* translationOf(const std::string& name) const;
    ModuleSource sourceOf(const std::string& name) const;

    Memory mMemory = makeIopMemory();
    IopInterpreter mInterpreter{mMemory};
    std::vector<Import> mImports;
    IopKernel mKernel;
    Symbols mSymbols;
    std::vector<IrxModule> mLoaded;
    std::span<TranslatedIrx* const> mTranslated;
    std::vector<std::string> mHostModules;
    // Loaded, so dispatch may run them.
    std::vector<TranslatedIrx*> mActive;
    u32 mNextBase = kIopModuleBase;
};

}  // namespace uee
