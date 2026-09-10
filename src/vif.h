#pragma once

#include "types.h"

#include <optional>

namespace uee {

class Gif;
class Memory;
struct Vu;

class Vif {
public:
    void attach(Memory& memory, Gif& gif, u32 unit, Vu* vu) {
        mMemory = &memory;
        mGif = &gif;
        mUnit = unit;
        mVu = vu;
        // What the boot rom's own use of the units leaves in the registers a
        // reset does not clear.
        mItop = unit == 0 ? 0xA3u : 0u;
        mTop = unit == 1 ? 0x1Cu : 0u;
        mStatus = unit == 1 ? kBootMark : 0u;
    }

    std::optional<u32> read(u32 address);
    bool write(u32 address, u32 value);

    void writeQuadword(u64 low, u64 high);

    // The two codes a DMA tag carries in its upper half.
    void writeTag(u64 codes);

private:
    u32 registerBase() const;
    u32 fifoBase() const;
    u32 memoryBase() const;
    u32 memorySize() const;
    u32 microBase() const;
    u32 microSize() const;

    void consume(u32 word);
    u32 fifoCount() const;
    bool wantsData() const;
    bool isTransferring() const;
    bool decodes(u32 command) const;
    void execute(u32 code);
    void uploadMicro(u32 word);
    void startMicro(u32 start);
    Vu& vu() const;
    void beginUnpack(u32 code);
    void sendToGif(u32 word);
    bool unpack(u32 word);
    bool needsData(u32 index) const;
    u32 writeCycle() const;
    bool isFillCycle(u32 index) const;
    void drainMaskedQuadwords();
    void flushQuadword();
    void writeLane(u32 quadword, u32 lane, u32 value);
    void writeMasked(u32 index, u32 lane, u32 value);
    u32 offsetBy(u32 lane, u32 value);
    u32 destinationOf(u32 index) const;
    u32 cycleOf(u32 index) const;

    Memory* mMemory = nullptr;
    Gif* mGif = nullptr;
    Vu* mVu = nullptr;
    u32 mUnit = 0;

    // The rom leaves the second unit having reached a mark.
    static constexpr u32 kBootMark = 1u << 6;

    u32 mStatus = 0;
    u32 mMark = 0;
    u32 mError = 0;
    u32 mNum = 0;
    u32 mFifoWords = 0;
    bool mFedByProcessor = false;
    bool mHoldingThree = false;
    // The unit powers up writing one quadword per cycle, and no reset returns
    // it to that.
    u32 mCycle = 0x0404;
    u32 mMode = 0;
    u32 mMask = 0;
    u32 mCode = 0;
    u32 mItops = 0;
    u32 mItop = 0;
    u32 mBase = 0;
    u32 mOffset = 0;
    u32 mTops = 0;
    u32 mTop = 0;
    bool mSecondBuffer = false;
    u32 mRow[4]{};
    u32 mColumn[4]{};

    u32 mRemaining = 0;
    u32 mFormat = 0;
    u32 mAddress = 0;
    u32 mLane = 0;
    u32 mWritten = 0;
    u32 mElement[4]{};
    u32 mFill[2]{};
    bool mUnsigned = false;
    bool mMasked = false;
    bool mStalled = false;
    bool mStallRequested = false;
    u32 mDirectWords = 0;
    u32 mDirectQuadword[4] = {};
    u32 mDirectFill = 0;
    u32 mMicroWords = 0;
    u32 mMicroWord = 0;
    u32 mPending = 0;
    u32* mPendingTarget = nullptr;
};

}  // namespace uee
