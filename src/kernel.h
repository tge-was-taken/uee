#pragma once

#include "alarm.h"
#include "budget.h"
#include "ee_mmio.h"
#include "sif.h"
#include "memory.h"
#include "perf.h"
#include "symbol_watch.h"
#include "patches.h"
#include "semaphores.h"
#include "symbols.h"
#include "thread.h"
#include "ee_context.h"
#include "types.h"

#include <algorithm>
#include <array>
#include <map>
#include <span>
#include <vector>
#include <optional>
#include <string>
#include <utility>

namespace uee {

enum class SyscallResult {
    Handled,
    Exited,
    Deadlocked,
    Unknown,
};

// Parked in $ra, so a handler's jr $ra lands here.
constexpr u32 kSyscallHandlerReturn = 0xF0000004;

struct GuestCall {
    u32 function = 0;
    u32 argument = 0;
    u32 secondArgument = 0;
    u32 thirdArgument = 0;
    // Zero outside a chain; a negative return cancels the rest of this one.
    u32 chain = 0;
};

class Kernel {
public:
    Kernel(EeContext& ctx, Memory& memory, const GameConfig& config = {},
           std::span<TranslatedIrx* const> iopModules = {});

    SyscallResult handleSyscall();
    SyscallResult callSyscall(s32 number);

    // A chain finishes in order, so a nested run must not start another.
    bool inHandler() const { return mHandlerDepth != 0; }
    void enterHandler() { ++mHandlerDepth; }
    void leaveHandler() { --mHandlerDepth; }

    // The run loop must execute this before the guest resumes.
    std::optional<GuestCall> takePendingCall();
    void completePendingCall(u32 result);

    void deliverInterrupts();

    bool isIdle() const { return mIdle; }
    void dispatchAfterInterrupt();
    void reportNoActiveThreads();
    void attachDisc(Disc& disc) { mSif.attachDisc(disc); }
    void attachFiles(std::filesystem::path root) { mSif.attachFiles(std::move(root)); }
    void attachAudio(AudioSink* audio) { mIop.attachAudio(audio); }
    void attachPad(PadSource* pad) { mIop.attachPad(pad); }
    void attachCard(CardStorage* card) { mIop.attachCard(card); }

    void advanceClock();

    Budget& budget() { return mBudget; }
    void stopAfter(u64 instructions) { mBudget.limit = instructions; }

    // True when the clock is worth advancing. Emitted code reports its own.
    bool countExecuted(u64 instructions) {
        mBudget.executed += instructions;
        // The interpreter already updated the performance registers.
        mBudget.counted = mBudget.executed;
        if (mBudget.executed < mBudget.nextService) {
            return false;
        }
        mBudget.nextService = mBudget.executed + Budget::kServiceInterval;
        return true;
    }

    u64 executed() const { return mBudget.executed; }

    bool reachedDeadline() const { return mBudget.stopped; }

    bool resumeFromSyscallHandler();

    usize currentThread() const { return mThreads.current(); }

    // For a wait on something outside the guest.
    void yieldCurrentThread();
    Symbols& symbols() { return mSymbols; }
    SymbolWatch& symbolWatch() { return mSymbolWatch; }
    Symbols& iopSymbols() { return mIop.symbols(); }
    Iop& iop() { return mIop; }
    std::span<const u32> watched() const { return mWatched; }

    void reportThreads() const;
    const std::string& readOutput() const { return mOutput; }


    EeMmio& mmio() { return mMmio; }

private:
    u32 readArgument(u32 index) const;
    void returnValue(u32 value);

    SyscallResult dispatch(u32 handler, bool interruptContext);

    u32 setupThread(u32 gp, u32 stack, u32 stackSize, u32 root);
    u32 setupHeap(u32 base, u32 size);

    s32 createThread(u32 param);
    s32 startThread(s32 id, u32 argument);
    void resetToDormant(usize id);
    s32 deleteThread(s32 id);
    s32 terminateThread(s32 id, bool interruptContext);
    s32 changeThreadPriority(s32 id, s32 priority, bool interruptContext);
    s32 rotateReadyQueue(s32 priority, bool interruptContext);
    s32 releaseWaitThread(s32 id, bool interruptContext);
    s32 referThreadStatus(s32 id, u32 info);
    s32 wakeupThread(s32 id, bool interruptContext);
    s32 cancelWakeupThread(s32 id);
    s32 suspendThread(s32 id, bool interruptContext);
    s32 resumeThread(s32 id, bool interruptContext);
    SyscallResult sleepThread();
    SyscallResult exitCurrentThread(bool andDelete);

    s32 createSemaphore(u32 param);
    s32 deleteSemaphore(s32 id, bool interruptContext);
    s32 signalSemaphore(s32 id, bool interruptContext);
    SyscallResult waitSemaphore(s32 id);
    s32 pollSemaphore(s32 id);
    s32 referSemaphoreStatus(s32 id, u32 info);

    s32 addIntcHandler(u32 cause, u32 func, s32 next, u32 argument);
    s32 enableIntc(u32 cause, bool enable);
    s32 enableDmac(u32 channel, bool enable);
    s32 addDmacHandler(u32 channel, u32 func, s32 next, u32 argument);

    void resetEE(u32 mask);
    u32 execPS2(u32 entry, u32 argument, u32 argc, u32 argv);
    void printUndefinedSyscall();
    u32 getCop0(u32 reg) const;
    void cpuConfig(u32 selector);
    void setKernelSegmentMode(u32 mode);
    s32 setVectorHandler(u32 index, u32 handler, u32 first, u32 last);
    s32 writeTlbEntry(u32 pageMask, u32 entryHi, u32 entryLo0, u32 entryLo1);
    void printKernel(u32 format);

    s32 setAlarm(u16 ticks, u32 callback, u32 argument);
    s32 releaseAlarm(s32 id);

    u32 sendSifDma(u32 transfers, u32 count);
    s32 deci2Call(u32 function, u32 arguments);
    u32 readSifRegister(u32 id) const;
    u32 writeSifRegister(u32 id, u32 value);

    void raiseFieldInterrupts();
    void queueHandlerChain(const std::vector<const IntrHandler*>& chain, u32 cause);
    void makeReadyAndPreempt(usize id);
    void blockCurrentAndDispatch(WaitType type);
    SyscallResult dispatchNext();
    SyscallResult rescheduleIfWanted(bool interruptContext);
    void loadThread(usize id);
    void switchTo(usize id);

    EeContext& mContext;
    Memory& mMemory;

    ThreadTable mThreads;
    SemaphoreTable mSemaphores;
    AlarmTable mAlarms;
    EeMmio mMmio;
    std::string mOutput;
    Symbols mSymbols;
    SymbolWatch mSymbolWatch;
    std::vector<u32> mWatched;
    u64 mIopBudgetPerHsync;
    Iop mIop;
    Sif mSif{mMemory, mMmio.sbus(), mOutput, mIop};

    u32 mHandlerCount = 0;
    u32 mRootThread = 0;
    u32 mStackBottom = 0;
    u32 mHeapEnd = 0;
    u32 mCpuTimerHandler = 0;
    u32 mFieldLine = 0;
    u64 mGsImr = 0xFF00;
    u32 mVSyncFlagAddress = 0;
    u32 mVSyncTickAddress = 0;
    u32 mGsHParam[4] = {};
    u32 mGsVParam = 0;
    u32 mGsCrtMode = 0;
    u32 mGsCrtInterlace = 0;
    u32 mGsCrtField = 0;
    std::array<u32, 16> mVectorHandlers = {};

    struct TlbEntry {
        u32 pageMask = 0;
        u32 entryHi = 0;
        u32 entryLo0 = 0;
        u32 entryLo1 = 0;
    };
    std::array<TlbEntry, 48> mTlb;
    bool mInVblank = false;
    SpeedMeter mSpeed{120};
    // Short enough that a guest polling a timer sees it move.

    u32 mHandlerDepth = 0;
    Budget mBudget;
    u32 mMachineType = 0;
    u32 mOsdConfig = 0;
    bool mIdle = false;

    struct SyscallReturn {
        u32 returnAddress = 0;
        u32 resumePc = 0;
    };
    std::optional<SyscallReturn> mSyscallReturn;

    std::array<u32, 0x20> mSifRegisters = {};
    u32 mDmaTransferId = 0;
    std::vector<GuestCall> mPendingCalls;
    u32 mChains = 0;
    u32 mRunningChain = 0;
    u32 mCallsHeld = 0;

};

}  // namespace uee
