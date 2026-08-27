#pragma once

#include "queue.h"
#include "ee_context.h"
#include "types.h"

#include <vector>

namespace uee {

constexpr usize kThreadCount = 256;
constexpr s32 kPriorityLevels = 129;
constexpr u32 kContextReserve = 0x2A0;
constexpr u32 kStackReserve = 0x20;

namespace status {
constexpr s32 kRun = 1;
constexpr s32 kReady = 2;
constexpr s32 kWait = 4;
constexpr s32 kSuspend = 8;
constexpr s32 kWaitSuspend = 12;
constexpr s32 kDormant = 16;
}  // namespace status

enum class WaitType : s32 {
    None = 0,
    Sleep = 1,
    Semaphore = 2,
};

// Mirrors EEThreadCB. The link is first so a thread can sit in a ready queue or
// on a semaphore wait list without a separate node.
struct ThreadControlBlock {
    Link link;
    s32 status = 0;
    u32 resumePc = 0;
    u32 context = 0;
    u32 globalPointer = 0;
    s16 initialPriority = 0;
    s16 currentPriority = 0;
    s32 waitType = 0;
    s32 waitId = 0;
    s32 wakeupCount = 0;
    u32 attr = 0;
    u32 option = 0;
    u32 entry = 0;
    s32 argc = 0;
    u32 argv = 0;
    u32 stack = 0;
    s32 stackSize = 0;
    u32 rootFunction = 0;
    u32 heapEnd = 0;

    EeContext saved;
};

class ThreadTable {
public:
    ThreadTable();

    ThreadControlBlock& at(usize id) { return mThreads[id]; }
    const ThreadControlBlock& at(usize id) const { return mThreads[id]; }

    usize current() const { return mCurrent; }
    void setCurrent(usize id) { mCurrent = id; }
    s32 count() const { return mCount; }
    bool isValid(s32 id) const { return id >= 0 && static_cast<usize>(id) < kThreadCount; }

    s32 allocate();
    void release(usize id);

    void makeReady(usize id);
    void removeFromReadyQueue(usize id);
    void rotateReadyQueue(s32 priority);
    // The first thread of the first non-empty level at or above the search hint.
    s32 chooseNext();

    bool rescheduleWanted() const { return mReschedule; }
    void requestReschedule() { mReschedule = true; }
    void clearReschedule() { mReschedule = false; }
    void raisePriorityHint(s16 priority);

    bool dispatchDisabled() const { return mDispatchDisabled; }
    void setDispatchDisabled(bool disabled) { mDispatchDisabled = disabled; }

private:
    usize indexOf(const Link* node) const;

    // Each block carries a full EeContext, so the table lives on the heap.
    std::vector<ThreadControlBlock> mThreads;
    std::vector<Link> mReadyQueue;
    Link mFreeList;
    usize mCurrent = 0;
    s32 mCount = 0;
    s32 mPriorityHint = 0;
    bool mReschedule = false;
    bool mDispatchDisabled = false;
};

}  // namespace uee
