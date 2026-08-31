#pragma once

#include "types.h"

#include <optional>

namespace uee {

class Gif;
class Memory;

class Vif {
public:
    void attach(Memory& memory, Gif& gif, u32 unit) {
        mMemory = &memory;
        mGif = &gif;
        mUnit = unit;
    }

    std::optional<u32> read(u32 address) const;
    bool write(u32 address, u32 value);

    void writeQuadword(u64 low, u64 high);

    // The two codes a DMA tag carries in its upper half.
    void writeTag(u64 codes);

private:
    u32 registerBase() const;
    u32 fifoBase() const;
    u32 memoryBase() const;
    u32 memorySize() const;

    void consume(u32 word);
    void execute(u32 code);
    void beginUnpack(u32 code);
    void sendToGif(u32 word);
    void unpack(u32 word);
    bool needsData(u32 index) const;
    void drainMaskedQuadwords();
    void flushQuadword();
    void writeLane(u32 quadword, u32 lane, u32 value);
    void writeMasked(u32 index, u32 lane, u32 value);
    u32 offsetBy(u32 lane, u32 value);
    u32 destinationOf(u32 index) const;
    u32 cycleOf(u32 index) const;

    Memory* mMemory = nullptr;
    Gif* mGif = nullptr;
    u32 mUnit = 0;

    u32 mStatus = 0;
    u32 mMark = 0;
    u32 mCycle = 0;
    u32 mMode = 0;
    u32 mMask = 0;
    u32 mCode = 0;
    u32 mItops = 0;
    u32 mBase = 0;
    u32 mOffset = 0;
    u32 mRow[4]{};
    u32 mColumn[4]{};

    u32 mRemaining = 0;
    u32 mFormat = 0;
    u32 mAddress = 0;
    u32 mLane = 0;
    u32 mWritten = 0;
    u32 mElement[4]{};
    bool mUnsigned = false;
    bool mMasked = false;
    bool mStalled = false;
    bool mStallRequested = false;
    u32 mDirectWords = 0;
    u32 mDirectQuadword[4] = {};
    u32 mDirectFill = 0;
    u32 mMicroWords = 0;
    u32 mPending = 0;
    u32* mPendingTarget = nullptr;
};

}  // namespace uee
