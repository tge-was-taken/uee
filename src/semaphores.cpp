#include "semaphores.h"

namespace uee {

// KERNEL: InitSemaphores @ 0x80004e60
SemaphoreTable::SemaphoreTable() {
    for (usize i = 0; i < kSemaphoreCount; ++i) {
        SemaphoreControlBlock& semaphore = mSemaphores[i];
        semaphore.count = -1;
        semaphore.freeNext = (i + 1 < kSemaphoreCount) ? static_cast<s32>(i + 1) : -1;
        initQueue(semaphore.waiters);
    }
}

// KERNEL: the free-list take in CreateSema @ 0x800049b8
s32 SemaphoreTable::allocate() {
    if (mFreeList < 0) {
        return -1;
    }
    const s32 id = mFreeList;
    mFreeList = mSemaphores[id].freeNext;
    mSemaphores[id].freeNext = -1;
    ++mCount;
    return id;
}

// KERNEL: the free-list push in iDeleteSema @ 0x80004a40
void SemaphoreTable::release(usize id) {
    mSemaphores[id].count = -1;
    mSemaphores[id].freeNext = mFreeList;
    mFreeList = static_cast<s32>(id);
    --mCount;
}

}  // namespace uee
