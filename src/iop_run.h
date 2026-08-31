#pragma once

#include "idle.h"

#include <span>
#include "iop_context.h"
#include "iop_translate.h"
#include "irx.h"
#include "stop.h"
#include "symbols.h"
#include "types.h"

#include <vector>

struct mco_coro;

namespace uee {

constexpr u32 kIopModuleBase = 0x00010000;
constexpr u32 kIopStackTop = 0x001F0000;
constexpr u32 kIopStackSize = 0x00010000;
constexpr u32 kIopModuleReturn = 0x0FFFFFF0;
constexpr u32 kIopThreadReturn = 0x0FFFFFE0;

void enterModule(IopContext& iop, const IrxModule& module);

// One coroutine per guest thread.
class IopScheduler {
public:
    IopScheduler(IopMachine& machine, const Symbols& symbols,
                 std::span<const u32> watched = {});
    ~IopScheduler();

    IopScheduler(const IopScheduler&) = delete;
    IopScheduler& operator=(const IopScheduler&) = delete;

    // Whether an idle processor may skip to whatever the clock will release.
    // A module entry may, because the EE waits on it; a slice the EE granted
    // may not, or the IOP runs ahead of the machine around it.
    enum class Skipping { Allowed, Blocked };

    Stop run(u64 budget, Skipping skipping = Skipping::Allowed);

private:
    static void enter(mco_coro* coroutine);
    void runThread();
    mco_coro* coroutineFor(usize thread);

    IopMachine& mMachine;
    Idle mIdle;
    std::vector<mco_coro*> mCoroutines;
    u64 mExecuted = 0;
    u64 mDeadline = 0;
    Stop mStop;
    bool mStopped = false;
};

}  // namespace uee
