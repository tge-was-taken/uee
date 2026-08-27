#pragma once

#include "queue.h"
#include "types.h"

#include <array>

namespace uee {

constexpr usize kSemaphoreCount = 256;

// Mirrors EESemaCB. A free or deleted block is marked by a negative count.
struct SemaphoreControlBlock {
    s32 count = -1;
    s32 maxCount = 0;
    u32 attr = 0;
    u32 option = 0;
    s32 waitCount = 0;
    Link waiters;
    s32 freeNext = -1;
};

class SemaphoreTable {
public:
    SemaphoreTable();

    SemaphoreControlBlock& at(usize id) { return mSemaphores[id]; }
    const SemaphoreControlBlock& at(usize id) const { return mSemaphores[id]; }

    bool isValid(s32 id) const { return id >= 0 && static_cast<usize>(id) < kSemaphoreCount; }
    bool isAlive(s32 id) const { return isValid(id) && mSemaphores[id].count >= 0; }

    s32 count() const { return mCount; }

    s32 allocate();
    void release(usize id);

private:
    std::array<SemaphoreControlBlock, kSemaphoreCount> mSemaphores;
    s32 mFreeList = 0;
    s32 mCount = 0;
};

}  // namespace uee
