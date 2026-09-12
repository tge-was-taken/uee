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
constexpr u32 kModulePageBytes = 0x1000;
constexpr u32 kIopStackTop = 0x001F0000;
constexpr u32 kIopStackSize = 0x00010000;
constexpr u32 kIopModuleReturn = 0x0FFFFFF0;
constexpr u32 kIopThreadReturn = 0x0FFFFFE0;
// Where guest code called from host code returns to.
constexpr u32 kIopNestedReturn = 0x0FFFFFD0;

// Where a module's argv sits; a module started with none sees a zero argc.
struct ModuleArguments {
    u32 count = 0;
    u32 vector = 0;
    u32 stackTop = kIopStackTop;
};

void enterModule(IopContext& iop, const IrxModule& module, const ModuleArguments& arguments = {});

// Host code running on a guest thread calls guest code the way guest code does,
// on that thread and its slices, and gets back what it returned. The thread's
// registers are as they were when the call comes back.
u32 callGuest(IopMachine& machine, u32 function, u32 gp, std::span<const u32> arguments);

// For host code that blocked the thread it runs on: returns once the thread runs
// again.
void waitUntilRunning(IopMachine& machine, usize thread);

// One coroutine per guest thread.
class IopScheduler {
public:
    IopScheduler(IopMachine& machine, const Symbols& symbols,
                 std::span<const u32> watched = {});
    ~IopScheduler();

    IopScheduler(const IopScheduler&) = delete;
    IopScheduler& operator=(const IopScheduler&) = delete;

    // A module entry may skip ahead because the EE waits on it; a granted
    // slice may not, or the IOP runs ahead of the machine around it.
    enum class Skipping { Allowed, Blocked };

    Stop run(u64 budget, Skipping skipping = Skipping::Allowed);

    // Runs the current thread until it reaches haltPc, giving up the slice
    // whenever it runs out rather than ending the call.
    Stop runNested(u32 haltPc);

    u64 getExecuted() const { return mExecuted; }

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
