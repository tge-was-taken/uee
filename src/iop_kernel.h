#pragma once

#include "iop_context.h"
#include "instruction.h"
#include "iop_thread.h"
#include "irx.h"
#include "memory.h"
#include "symbols.h"
#include "types.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uee {

struct HeapRange {
    u32 start = 0;
    u32 end = 0;
};

// What sceSifRegisterRpc recorded. The handler runs with the gp its module was
// linked at, which is why that is kept alongside it.
struct IopRpcServer {
    u32 id = 0;
    u32 data = 0;
    u32 handler = 0;
    u32 buffer = 0;
    u32 gp = 0;
};

class IopKernel {
public:
    // The main memory is where sceSifSetDma moves what a module hands the EE.
    IopKernel(IopContext& iop, Memory& memory, Memory& mainMemory,
              const std::vector<Import>& imports, HeapRange heap, std::string& console);

    // False stops the run: nothing here answers for that import.
    bool call(u32 index);



    usize currentThread() const { return mCurrent; }
    bool isCurrent(usize thread) const;
    bool isIdle() const;
    void reportThreads(const Symbols& symbols) const;
    void exitCurrentThread();

    // The EE's way in stops being runnable; whatever guest thread was running
    // keeps its place.
    void exitHostThread();

    // Gives the processor to the thread the host runs its own calls on, which
    // is also the one a module entry gets.
    void enterHostThread();

    // What the EE asks for through the FILEIO heap server, which is the same
    // pool sysmem hands to the modules loaded here.
    u32 allocateMemory(u32 size);
    u32 allocateMemory(u32 mode, u32 size, u32 address);
    bool freeMemory(u32 address);

    // Releases what WaitVblankEnd parked, which is the only clock the modules
    // loaded here run on.
    void raiseVblankStart();
    void raiseVblankEnd();
    void advanceTime(u64 instructions);
    void advanceTimeTo(u64 cycles);
    std::optional<u64> nextDelay() const;
    std::vector<IopHandlerCall> takePendingHandlers();
    void raiseInterrupt(u32 line);
    void raiseInterruptAfter(u32 line, u64 cycles);
    usize enterInterruptThread();
    void leaveInterruptThread(usize interrupted);

    u32 sequence() const { return mSequence; }
    // Undoes a module that could not be run: the threads it started have no
    // code left worth running.
    void dropThreadsAbove(u32 sequence);
    std::string nameOf(u32 index) const;

    const std::vector<IopRpcServer>& rpcServers() const { return mRpcServers; }

private:
    using Handler = void (IopKernel::*)();

    struct Service {
        std::string_view module;
        u32 id;
        std::string_view symbol;
        Handler run;
        // Answered without the subsystem behind it.
        bool isStub = false;
    };

    static const Service* lookup(const Import& import);

    u32 argument(u32 index);
    void returnValue(u32 value);
    u32 stringLength(u32 address);
    std::string readString(u32 address);
    void writeString(u32 address, const std::string& text);
    std::string format(u32 text, u32 firstArgument);

    struct Block {
        u32 start = 0;
        u32 size = 0;
        bool used = false;
    };

    u32 allocateBlock(u32 mode, u32 size, u32 wanted);
    bool freeBlock(u32 address);
    std::vector<Block>::iterator blockAt(u32 address);
    std::vector<Block>::iterator blockContaining(u32 address);

    struct Interrupt {
        u32 handler = 0;
        u32 argument = 0;
        u32 gp = 0;
    };

    struct VblankHandler {
        u32 handler = 0;
        u32 argument = 0;
        u32 gp = 0;
        s32 priority = 0;
    };

    std::vector<VblankHandler>& vblankHandlersFor(bool atEnd);
    void queueHandlers(const std::vector<VblankHandler>& handlers);

    void cpuSuspendIntr();
    void cpuResumeIntr();
    void cpuEnableIntr();
    void cpuDisableIntr();
    void queryIntrContext();
    void registerIntrHandler();
    void releaseIntrHandler();
    void enableIntr();
    void disableIntr();

    void allocSysMemory();
    void freeSysMemory();
    void queryMemSize();
    void queryMaxFreeMemSize();
    void queryTotalFreeMemSize();
    void queryBlockSize();
    void queryBlockTopAddress();

    // The packing is this runtime's own: THREADMAN packs a control block
    // address, which nothing outside it can reproduce.
    static u32 makeHandle(usize index, u32 sequence);
    static usize handleIndex(u32 handle);
    static u32 handleSequence(u32 handle);

    IopThread* threadFor(u32 handle);
    IopSemaphore* semaphoreFor(u32 handle);
    IopEventFlag* eventFlagFor(u32 handle);

    void switchTo(usize thread);
    u32 waitingOn(IopWait wait, u32 id) const;
    void readyThread(usize thread);
    void blockCurrentThread(IopWait wait, u32 id);
    void reschedule();

    void flushCache();
    void registerLibraryEntries();
    void setDmaPriority();
    void enableDmaChannel();
    void requestDma();
    void startDma();
    void setRebootTimeLibraryHandlingMode();
    void setMcDevIdHandler();
    void authCard();
    void setMcCommandHandler();
    void kprintf();

    void createThread();
    void deleteThread();
    void startThread();
    void exitThread();
    void terminateThread();
    void getThreadId();
    void referThreadStatus();
    void changeThreadPriority();
    void sleepThread();
    void wakeupThread();
    void cancelWakeupThread();
    void delayThread();
    void getSystemTime();
    void usecToSysClock();
    void sysClockToUsec();

    void createSema();
    void deleteSema();
    void signalSema();
    void waitSema();
    void pollSema();
    void referSemaStatus();

    void createEventFlag();
    void deleteEventFlag();
    void setEventFlag();
    void clearEventFlag();
    void waitEventFlag();
    void pollEventFlag();
    void referEventFlagStatus();

    void printf();
    void sprintf();
    void lookCtypeTable();
    void memcmp();
    void memcpy();
    void memset();
    void bzero();
    void strcat();
    void strcpy();
    void strlen();
    void strncmp();
    void strncpy();
    void strtol();

    void sifInit();
    void sifSetDma();
    void sifCheckInit();
    void sifInitRpc();
    void sifBindRpc();
    void sifCallRpc();
    void sifDmaStat();
    void sifRegisterRpc();
    void sifSetRpcQueue();
    void sifRpcLoop();
    void sifRemoveRpc();
    void sifRemoveRpcQueue();

    void waitVblankEnd();
    void registerVblankHandler();
    void releaseVblankHandler();

    void sdInit();
    void sdSetParam();
    void sdGetParam();
    void sdSetSwitch();
    void sdGetSwitch();
    void sdSetAddr();
    void sdGetAddr();
    void sdSetCoreAttr();
    void sdGetCoreAttr();
    void sdConvertPitch();
    void sdProcBatch();
    void sdVoiceTrans();
    void sdBlockTrans();
    void sdTransStatus();
    void sdSetEffectAttr();
    void sdGetEffectAttr();
    void sdClearEffectWorkArea();
    void sdSetTransIntrHandler();
    void sdSetSpu2IntrHandler();
    void sdGetTransIntrHandlerArgument();
    void sdAccepted();

    void allocHardTimer();
    void freeHardTimer();
    void setTimerHandler();
    void setupHardTimer();
    void startHardTimer();
    void stopHardTimer();

    IopHardTimer* hardTimerFor(u32 timid);

    IopContext& mIop;
    Memory& mMemory;
    Memory& mMainMemory;
    const std::vector<Import>& mImports;
    std::vector<const Service*> mServices;
    std::vector<Block> mBlocks;
    std::vector<IopThread> mThreads;
    std::vector<IopSemaphore> mSemaphores;
    std::vector<IopEventFlag> mEventFlags;
    usize mCurrent = 0;
    u32 mSequence = 0;
    u64 mCycles = 0;
    std::set<const Service*> mStubReported;
    Interrupt mInterrupts[64];
    bool mInterruptsEnabled = true;
    std::string& mConsole;
    std::vector<IopRpcServer> mRpcServers;
    u32 mTransIntrHandler = 0;
    u32 mTransIntrArgument = 0;
    u32 mSpu2IntrHandler = 0;
    u32 mSpu2IntrArgument = 0;

    std::map<u32, u32> mSdParams;
    std::map<u32, u32> mSdSwitches;
    std::map<u32, u32> mSdAddresses;
    std::map<u32, u32> mSdCoreAttrs;

    IopHardTimer mHardTimers[kHardTimers];
    
    std::vector<VblankHandler> mVblankStartHandlers;
    std::vector<VblankHandler> mVblankEndHandlers;
    std::vector<IopHandlerCall> mPendingHandlers;
    std::vector<std::pair<u64, u32>> mDeferredInterrupts;
};

}  // namespace uee
