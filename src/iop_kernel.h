#pragma once

#include "iop_context.h"
#include "instruction.h"
#include "iop_thread.h"
#include "irx.h"
#include "memory.h"
#include "types.h"

#include <string>
#include <vector>

namespace uee {

struct HeapRange {
    u32 start = 0;
    u32 end = 0;
};

class IopKernel {
public:
    IopKernel(IopContext& iop, Memory& memory, std::vector<Import> imports, HeapRange heap);

    // False stops the run: nothing here answers for that import.
    bool call(u32 index);

    usize currentThread() const { return mCurrent; }
    bool isIdle() const;
    void exitCurrentThread();
    std::string nameOf(u32 index) const;

    const std::string& readOutput() const { return mConsole; }

private:
    using Service = void (IopKernel::*)();

    static Service lookup(const Import& import);

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

    u32 allocateBlock(u32 mode, u32 size);
    bool freeBlock(u32 address);
    std::vector<Block>::iterator blockAt(u32 address);
    std::vector<Block>::iterator blockContaining(u32 address);

    struct Interrupt {
        u32 handler = 0;
        u32 argument = 0;
    };

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

    IopContext& mIop;
    Memory& mMemory;
    std::vector<Import> mImports;
    std::vector<Service> mServices;
    std::vector<Block> mBlocks;
    std::vector<IopThread> mThreads;
    std::vector<IopSemaphore> mSemaphores;
    std::vector<IopEventFlag> mEventFlags;
    usize mCurrent = 0;
    u32 mSequence = 0;
    u64 mMicroseconds = 0;
    Interrupt mInterrupts[64];
    bool mInterruptsEnabled = true;
    std::string mConsole;
};

}  // namespace uee
