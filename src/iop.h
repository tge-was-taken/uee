#pragma once

#include "iop_interpreter.h"
#include "iop_kernel.h"
#include "iop_mmio.h"
#include "iop_run.h"
#include "irx.h"
#include "memory.h"
#include "symbols.h"
#include "types.h"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace uee {

// Modules live from here up to the heap sysmem hands out.
constexpr u32 kIopHeapBase = 0x00100000;

// Whether this runtime answers for a module or the game's own copy runs.
enum class ModuleSource { Host, Guest };

// Driven by the EE: code runs while a call is outstanding, not on its own clock.
class Iop {
public:
    // Translated modules keep their bases; everything else loads above.
    Iop(Memory& mainMemory, std::string& console,
        std::span<TranslatedIrx* const> translated = {},
        std::span<const std::string> hostModules = {}, std::span<const u32> watched = {});

    // What the module's start returned, or nothing when it did not start. A
    // module the host answers is not loaded and reports zero.
    std::optional<s32> loadModule(const std::string& name, std::span<const u8> image,
                                  std::span<const std::string> arguments = {});
    // The same, for a loader running on an IOP thread: the module starts on that
    // thread, as MODLOAD starts it on the thread that asked.
    std::optional<s32> loadModuleOnThread(const std::string& name, std::span<const u8> image,
                                          std::span<const std::string> arguments = {});
    void addHostServer(u32 id, SifServer& server);
    ModuleSource sourceOf(const std::string& name) const;
    IopKernel& getKernel() { return mKernel; }

    u32 allocateMemory(u32 size) { return mKernel.allocateMemory(size); }
    u32 allocateMemory(u32 mode, u32 size, u32 address) {
        return mKernel.allocateMemory(mode, size, address);
    }
    bool freeMemory(u32 address) { return mKernel.freeMemory(address); }
    void attachAudio(AudioSink* audio) { mMmio.attachAudio(audio); }
    void attachPad(PadSource* pad) { mMmio.attachPad(pad); }
    void attachCard(CardStorage* card) { mMmio.attachCard(card); }
    void startVblank();
    void endVblank();
    // Brings the IOP's clock cycles further along with the EE's and runs it for
    // up to instructions.
    void advance(u64 cycles, u64 instructions);
    u64 getCycles() const { return mKernel.getCycles(); }
    // Runs what the EE just handed over for as long as a call waits on it, as
    // fast as it will go rather than on the EE's clock.
    void runRequested();
    void reportThreads() const { mKernel.reportThreads(mSymbols); }
    void reportRegisters() const;

    Symbols& symbols() { return mSymbols; }

    Memory& memory() { return mMemory; }

private:
    Stop enter(IopScheduler::Skipping skipping = IopScheduler::Skipping::Allowed);
    bool sync(IopScheduler::Skipping skipping = IopScheduler::Skipping::Allowed);
    bool dispatchPendingHandlers();
    void runHandler(const IopHandlerCall& call);

    ModuleArguments writeArguments(const std::string& name, std::span<const std::string> arguments,
                                   u32 top);
    std::optional<IrxModule> linkModule(const std::string& name, std::span<const u8> image);

    TranslatedIrx* translationOf(const std::string& name) const;

    Memory mMemory = makeIopMemory();
    IopMmio mMmio;
    IopInterpreter mInterpreter{mMemory};
    std::vector<Import> mImports;

    IopKernel mKernel;
    Symbols mSymbols;
    std::vector<u32> mWatched;
    std::vector<IrxModule> mLoaded;
    std::span<TranslatedIrx* const> mTranslated;
    std::vector<std::string> mHostModules;
    // Loaded, so dispatch may run them.
    std::vector<TranslatedIrx*> mActive;
    void indexModulePages();

    std::vector<TranslatedIrx*> mModulePages;
    u32 mNextBase = kIopModuleBase;
    IopMachine mMachine{mInterpreter, mMemory, mKernel, mActive};
    IopScheduler mScheduler{mMachine, mSymbols, mWatched};
    bool mAdvancing = false;
    // Where the EE's clock has taken the IOP's, in IOP cycles.
    u64 mClockTarget = 0;
    // The thread entry of the servers answered here: one call into sceSifRpcLoop.
    u32 mRpcLoopEntry = 0;
    u64 mHandlersRun = 0;
};

}  // namespace uee
