#pragma once

#include "memory.h"
#include "ee_context.h"
#include "types.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace uee {

enum class SyscallResult {
    Handled,
    Exited,
    Deadlocked,
    Unknown,
};

struct GuestCall {
    u32 function = 0;
    u32 argument = 0;
};

class Kernel {
public:
    Kernel(EeContext& ee, Memory& memory);

    SyscallResult handleSyscall();

    // The guest callback an RPC reply owes its caller, which the run loop has
    // to execute before the guest resumes.
    std::optional<GuestCall> takePendingCall();

    usize currentThread() const { return mCurrent; }

    const std::string& readOutput() const { return mOutput; }

private:
    enum class ThreadState {
        Dormant,
        Ready,
        Waiting,
        Dead,
    };

    struct Thread {
        EeContext context;
        ThreadState state = ThreadState::Dormant;
        s32 priority = 0;
        s32 waitingOn = 0;
    };

    struct Semaphore {
        s32 count = 0;
        s32 maxCount = 0;
        bool isAlive = false;
    };

    u32 readArgument(u32 index) const;
    void returnValue(u32 value);

    u32 setupThread(u32 gp, u32 stack, u32 stackSize, u32 root);
    u32 setupHeap(u32 base, u32 size);
    s32 createThread(u32 param);
    s32 startThread(s32 id, u32 argument);
    SyscallResult exitCurrentThread();

    s32 createSemaphore(u32 param);
    SyscallResult waitSemaphore(s32 id);
    void signalSemaphore(s32 id);

    u32 sendSifDma(u32 transfers, u32 count);
    u32 readSifRegister(u32 id) const;
    void writeSifRegister(u32 id, u32 value);
    s32 writeFile(u32 argument);

    usize chooseReadyThread() const;
    void switchTo(usize index);
    void reschedule();
    Semaphore& findSemaphore(s32 id);

    EeContext& mContext;
    Memory& mMemory;

    std::vector<Thread> mThreads;
    std::vector<Semaphore> mSemaphores;
    usize mCurrent = 0;

    u32 mHandlerCount = 0;
    u32 mRootThread = 0;
    u32 mStackBottom = 0;
    u32 mHeapEnd = 0;

    std::map<u32, u32> mSifRegisters;
    u32 mDmaTransferId = 0;
    std::optional<GuestCall> mPendingCall;

    std::string mOutput;
};

}  // namespace uee
