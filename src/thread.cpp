#include "thread.h"

#include <cstddef>

namespace uee {

static_assert(offsetof(ThreadControlBlock, link) == 0,
              "the queue links must start the block so a Link* names a thread");

// KERNEL: InitThreads @ 0x80004ec0
ThreadTable::ThreadTable() : mThreads(kThreadCount), mReadyQueue(kPriorityLevels) {
    for (Link& level : mReadyQueue) {
        initQueue(level);
    }
    initQueue(mFreeList);
    for (ThreadControlBlock& thread : mThreads) {
        thread.status = 0;
        insertHead(mFreeList, thread.link);
    }
}

usize ThreadTable::indexOf(const Link* node) const {
    return static_cast<usize>(reinterpret_cast<const ThreadControlBlock*>(node) - mThreads.data());
}

// KERNEL: QueueRemoveTail on k_FreeThreadList in CreateThread @ 0x80003c50
s32 ThreadTable::allocate() {
    Link* node = removeTail(mFreeList);
    if (node == nullptr) {
        return -1;
    }
    ++mCount;
    return static_cast<s32>(indexOf(node));
}

// KERNEL: FreeThreadCB @ 0x800059d8
void ThreadTable::release(usize id) {
    --mCount;
    insertHead(mFreeList, mThreads[id].link);
}

void ThreadTable::raisePriorityHint(s16 priority) {
    if (priority < mPriorityHint) {
        mPriorityHint = priority;
    }
}

// KERNEL: MakeThreadReady @ 0x80005a58
void ThreadTable::makeReady(usize id) {
    ThreadControlBlock& thread = mThreads[id];
    thread.status = status::kReady;
    const s16 priority = thread.currentPriority;
    raisePriorityHint(priority);
    insertHead(mReadyQueue[static_cast<usize>(priority)], thread.link);
}

// KERNEL: RemoveFromReadyQueue @ 0x80005a18
void ThreadTable::removeFromReadyQueue(usize id) {
    const s32 state = mThreads[id].status;
    if (state == status::kRun || state == status::kReady) {
        unlink(mThreads[id].link);
    }
}

// KERNEL: iRotateThreadReadyQueue @ 0x80004380
void ThreadTable::rotateReadyQueue(s32 priority) {
    uee::rotate(mReadyQueue[static_cast<usize>(priority)]);
}

// KERNEL: the ready-queue scan in DispatchNextThread @ 0x80003940
bool ThreadTable::anyWaiting() const {
    for (const ThreadControlBlock& thread : mThreads) {
        if ((thread.status & (status::kWait | status::kSuspend)) != 0) {
            return true;
        }
    }
    return false;
}

s32 ThreadTable::chooseNext() {
    for (s32 priority = mPriorityHint; priority < kPriorityLevels; ++priority) {
        Link& level = mReadyQueue[static_cast<usize>(priority)];
        if (isEmpty(level)) {
            continue;
        }
        mPriorityHint = priority;
        return static_cast<s32>(indexOf(level.prev));
    }
    return -1;
}

}  // namespace uee
