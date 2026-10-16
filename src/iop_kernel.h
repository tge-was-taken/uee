#pragma once

#include "iop_context.h"
#include "instruction.h"
#include "iop_thread.h"
#include "irx.h"
#include "memory.h"
#include "sif_server.h"
#include "symbols.h"
#include "types.h"

#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uee {

struct HeapRange {
    u32 start = 0;
    u32 end = 0;
};

// A command packet for the EE, and the data sent ahead of it.
struct SifPacket {
    std::vector<u32> words;
    u32 dataSource = 0;
    u32 dataDestination = 0;
    u32 dataSize = 0;
};

// What the EE's call packet tells a server a module registered.
struct SifRpcCall {
    u32 packet = 0;
    u32 client = 0;
    u32 number = 0;
    u32 sendSize = 0;
    u32 receive = 0;
    u32 receiveSize = 0;
    u32 receiveMode = 0;
    u32 server = 0;
    // Where the EE sent the arguments from.
    u32 sendSource = 0;
};

class IopMmio;
struct IopMachine;

class IopKernel {
public:
    // The main memory is where sceSifSetDma moves what a module hands the EE.
    IopKernel(IopContext& iop, Memory& memory, Memory& mainMemory,
              const std::vector<Import>& imports, HeapRange heap, std::string& console);

    // False stops the run: nothing here answers for that import.
    bool call(u32 index);



    usize currentThread() const { return mCurrent; }
    bool isCurrent(usize thread) const;
    // A thread made ready from outside any thread has not been switched to yet.
    void switchToRunnable();
    bool isIdle() const;
    void reportThreads(const Symbols& symbols) const;
    void exitCurrentThread();

    // The EE's way in stops being runnable; whatever guest thread was running
    // keeps its place.
    void exitHostThread();

    // Gives the processor to the thread the host runs its own calls on, which
    // is also the one a module entry gets.
    void enterHostThread();

    // What guest calls made from host code run on.
    void attachMachine(IopMachine& machine) { mMachine = &machine; }

    // Nothing carries a request from the IOP to the EE yet, so a host thread
    // that made one can never go on.
    bool isHostWaitingOnEe() const;

    // What the EE asks for through the FILEIO heap server, which is the same
    // pool sysmem hands to the modules loaded here.
    u32 allocateMemory(u32 size);
    u32 allocateMemory(u32 mode, u32 size, u32 address);
    bool freeMemory(u32 address);

    // Releases what WaitVblankEnd parked, which is the only clock the modules
    // loaded here run on.
    void raiseVblankStart();
    void raiseVblankEnd();
    void attachDevices(IopMmio& devices) { mDevices = &devices; }
    void advanceTime(u64 instructions);
    void advanceTimeTo(u64 cycles);
    u64 getCycles() const { return mCycles; }
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

    // SIFCMD's RPC, on the IOP's side of the SIF: the servers the loaded modules
    // registered, and the calls the modules make to the EE.
    u32 findRpcServer(u32 id) const;
    bool isRpcServer(u32 server) const;
    u32 getRpcServerBuffer(u32 server) const;
    u32 getRpcServerClientBuffer(u32 server) const;
    void receiveRpcCall(const SifRpcCall& call);
    void receiveRpcEnd(std::span<const u32> packet);
    std::vector<SifPacket> takePacketsForEe();
    bool hasUnansweredCalls() const { return !mUnanswered.empty(); }
    // A server this runtime answers, set up the way a module sets up its own: a
    // queue, a record and a buffer in IOP memory, and a thread that enters
    // sceSifRpcLoop at loopEntry.
    void addHostServer(u32 id, SifServer& server, u32 loopEntry);

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
    static std::span<const Service> services();

public:
    // Where a host-answered import sits in the service table, which the
    // translator resolves once instead of the run resolving it every call.
    static std::optional<u32> findService(std::string_view module, u32 id);
    void callService(u32 index);

private:

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
    void queryBootMode();
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
    void runSifTransfers(u32 transfers, u32 count);
    void sifSetDma();
    void sifSetDmaIntr();
    void sifGetOtherData();
    void sifCheckInit();
    void sifInitRpc();
    void sifBindRpc();
    void sifCallRpc();
    void sifCheckStatRpc();
    void sifDmaStat();
    void sifRegisterRpc();
    void sifSetRpcQueue();
    void sifGetNextRequest();
    void sifExecRequest();
    void sifRpcLoop();
    void sifRemoveRpc();
    void sifRemoveRpcQueue();

    // The records of the servers answered here, and where the EE's arguments
    // came from, which their handlers read.
    struct HostServer {
        SifServer* server = nullptr;
        u32 id = 0;
        u32 sendSource = 0;
    };

    u32 addThread(u32 entry, s32 priority, u32 stackSize, u32 attr, u32 option);
    void runThread(IopThread& thread, u32 argument, u32 gp);

    u32 allocatePacket();
    void freePacket(u32 packet);
    std::vector<u32> readPacket(u32 packet) const;
    void sendToEe(u32 command, std::vector<u32> words, u32 dataSource, u32 dataDestination,
                  u32 dataSize);
    void linkQueue(u32 queue, u32 thread);
    u32 unlinkServer(u32 queue, u32 server);
    u32 takeRequest(u32 queue);
    void execRequest(u32 server);
    s32 answerOnHost(u32 server, const HostServer& host);
    void sendReply(u32 server, u32 reply);
    void sleepCurrentThread();
    void wakeThread(u32 handle);

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

    u64 findNextDeadline() const;
    void setNextDeadline(u64 at);

    IopContext& mIop;
    Memory& mMemory;
    Memory& mMainMemory;
    const std::vector<Import>& mImports;
    std::vector<const Service*> mServices;
    std::vector<Block> mBlocks;
    // Never later than the first deadline with work behind it, so a clock short
    // of it has nothing to do. Zero asks for the scan that makes it exact again.
    u64 mNextDeadline = 0;
    std::vector<IopThread> mThreads;
    std::vector<IopSemaphore> mSemaphores;
    std::vector<IopEventFlag> mEventFlags;
    usize mCurrent = 0;
    u32 mSequence = 0;
    u64 mCycles = 0;
    IopMachine* mMachine = nullptr;
    IopMmio* mDevices = nullptr;
    std::set<const Service*> mStubReported;
    Interrupt mInterrupts[64];
    bool mInterruptsEnabled = true;
    std::string& mConsole;
    // SIFCMD keeps its queues on one list, and its packets in one pool.
    u32 mRpcQueues = 0;
    u32 mPacketPool = 0;
    u32 mRpcId = 0;
    std::vector<SifPacket> mToEe;
    // Servers holding a call from the EE they have not answered.
    std::set<u32> mUnanswered;
    std::map<u32, HostServer> mHostServers;
    // The gp a client's end function runs with, from the thread that called.
    std::map<u32, u32> mEndFunctionGp;
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
