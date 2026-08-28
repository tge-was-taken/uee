#include "vif.h"

#include "gif.h"
#include "memory.h"

namespace uee {
namespace {

constexpr u32 kRegisterSpan = 0x180;
constexpr u32 kFifoSpan = 0x1000;

constexpr u32 kStat = 0x00, kFbrst = 0x10, kErr = 0x20, kMark = 0x30;
constexpr u32 kCycle = 0x40, kMode = 0x50, kNum = 0x60, kMask = 0x70;
constexpr u32 kCode = 0x80, kItops = 0x90, kBase = 0xA0, kOffset = 0xB0;
constexpr u32 kTops = 0xC0, kItop = 0xD0, kTop = 0xE0;
constexpr u32 kRow = 0x100, kColumn = 0x140;

namespace code {
constexpr u32 kNop = 0x00, kStcycl = 0x01, kOffset = 0x02, kBase = 0x03;
constexpr u32 kItop = 0x04, kStmod = 0x05, kMskpath3 = 0x06, kMark = 0x07;
constexpr u32 kStmask = 0x20, kStrow = 0x30, kStcol = 0x31;
}  // namespace code

constexpr u32 kDataWait = 1;
constexpr u32 kDecoding = 2;
constexpr u32 kMarkFlag = 1u << 6;
constexpr u32 kInterruptStall = 1u << 10;
constexpr u32 kInterruptFlag = 1u << 11;

constexpr u32 kCodeInterrupt = 1u << 31;

constexpr u32 kUnpack = 0x60;
constexpr u32 kUnpackMask = 0x60;
constexpr u32 kUnpackMasked = 0x10;
constexpr u32 kUnpackFive = 0xF;

constexpr u32 kOffsetMode = 1;
constexpr u32 kDifferenceMode = 2;
constexpr u32 kRowMode = 3;

constexpr u32 kCycleLimit = 4;

constexpr u32 lanesOf(u32 format) {
    return ((format >> 2) & 3u) + 1u;
}

constexpr u32 widthOf(u32 format) {
    switch (format & 3u) {
        case 0: return 32;
        case 1: return 16;
        case 2: return 8;
        default: return 16;  // the 5-5-5-1 form packs a quadword into one halfword
    }
}

}  // namespace

u32 Vif::registerBase() const { return mUnit == 0 ? 0x10003800u : 0x10003C00u; }
u32 Vif::fifoBase() const { return mUnit == 0 ? 0x10004000u : 0x10005000u; }
u32 Vif::memoryBase() const { return mUnit == 0 ? 0x11004000u : 0x1100C000u; }
u32 Vif::memorySize() const { return mUnit == 0 ? 4u * 1024u : 16u * 1024u; }

std::optional<u32> Vif::read(u32 address) const {
    if (address < registerBase() || address >= registerBase() + kRegisterSpan) {
        return std::nullopt;
    }
    switch (address - registerBase()) {
        case kStat:
            return (mStatus & ~3u) |
                   (mStalled ? kDecoding : (mRemaining != 0 ? kDataWait : 0u));
        case kMark: return mMark;
        case kCycle: return mCycle;
        case kMode: return mMode;
        // The first unit's count is eight bits wide, so a full 256 reads back
        // as zero there; the second unit has room for it.
        case kNum: return mUnit == 0 ? (mRemaining & 0xFFu) : mRemaining;
        case kMask: return mMask;
        case kCode: return mCode;
        case kItops: return mItops;
        case kBase: return mBase;
        case kOffset: return mOffset;
        case kTops: return mBase;
        case kItop: return mItops;
        case kTop: return mBase;
        case kRow + 0x00: return mRow[0];
        case kRow + 0x10: return mRow[1];
        case kRow + 0x20: return mRow[2];
        case kRow + 0x30: return mRow[3];
        case kColumn + 0x00: return mColumn[0];
        case kColumn + 0x10: return mColumn[1];
        case kColumn + 0x20: return mColumn[2];
        case kColumn + 0x30: return mColumn[3];
        default: return 0;
    }
}

bool Vif::write(u32 address, u32 value) {
    if (address >= fifoBase() && address < fifoBase() + kFifoSpan) {
        consume(value);
        return true;
    }
    if (address < registerBase() || address >= registerBase() + kRegisterSpan) {
        return false;
    }
    switch (address - registerBase()) {
        // Writing one clears the sticky bits rather than setting them.
        case kStat: mStatus &= ~value; return true;
        case kFbrst:
            if (value & 1u) {
                mStatus = 0;
                mStalled = false;
                mStallRequested = false;
                mRemaining = 0;
                mPending = 0;
                mPendingTarget = nullptr;
            }
            return true;
        case kErr: return true;
        case kMark: mMark = value; mStatus &= ~kMarkFlag; return true;
        default: return true;
    }
}

void Vif::writeQuadword(u64 low, u64 high) {
    consume(static_cast<u32>(low));
    consume(static_cast<u32>(low >> 32));
    consume(static_cast<u32>(high));
    consume(static_cast<u32>(high >> 32));
}

// A code that asked for an interrupt still gets to finish: the stall only
// takes hold once the words it owns have been read.
void Vif::consume(u32 word) {
    if (mStalled) {
        return;
    }
    if (mPendingTarget != nullptr) {
        *mPendingTarget++ = word;
        if (--mPending == 0) {
            mPendingTarget = nullptr;
        }
    } else if (mRemaining != 0) {
        unpack(word);
    } else {
        execute(word);
    }
    if (mStallRequested && mPendingTarget == nullptr && mRemaining == 0) {
        mStalled = true;
    }
}

void Vif::execute(u32 code) {
    mCode = code;
    const u32 command = (code >> 24) & 0x7Fu;
    const u32 immediate = code & 0xFFFFu;

    // A code that asks for an interrupt raises one and stops the unit until the
    // guest resets it. Mark is the exception: it reports itself and carries on.
    if ((code & kCodeInterrupt) != 0 && command != code::kMark) {
        mStatus |= kInterruptStall | kInterruptFlag;
        mStallRequested = true;
    }

    if ((command & kUnpackMask) == kUnpack) {
        beginUnpack(code);
        return;
    }
    switch (command) {
        case code::kNop: return;
        case code::kStcycl: mCycle = immediate; return;
        case code::kOffset: mOffset = immediate & 0x3FFu; return;
        case code::kBase: mBase = immediate & 0x3FFu; return;
        case code::kItop: mItops = immediate & 0x3FFu; return;
        case code::kStmod: mMode = immediate & 3u; return;
        case code::kMskpath3:
            if (mUnit == 1) {
                mGif->maskPath3((immediate & 0x8000u) != 0);
            }
            return;
        case code::kMark:
            mMark = immediate;
            mStatus |= kMarkFlag;
            if ((code & kCodeInterrupt) != 0) {
                mStatus |= kInterruptFlag;
            }
            return;
        case code::kStmask: mPending = 1; mPendingTarget = &mMask; return;
        case code::kStrow: mPending = 4; mPendingTarget = mRow; return;
        case code::kStcol: mPending = 4; mPendingTarget = mColumn; return;
        default: return;
    }
}

void Vif::beginUnpack(u32 code) {
    const u32 command = (code >> 24) & 0x7Fu;
    const u32 count = (code >> 16) & 0xFFu;
    const u32 immediate = code & 0xFFFFu;

    mFormat = command & 0xFu;
    mUnsigned = ((immediate >> 14) & 1u) != 0;
    mAddress = immediate & 0x3FFu;
    if (mUnit == 1 && ((immediate >> 15) & 1u) != 0) {
        mAddress += mBase;
    }
    mRemaining = count == 0 ? 256u : count;
    mMasked = (command & kUnpackMasked) != 0;
    mLane = 0;
    mWritten = 0;
    drainMaskedQuadwords();
}

u32 Vif::destinationOf(u32 index) const {
    const u32 cycle = mCycle & 0xFFu;
    const u32 write = (mCycle >> 8) & 0xFFu;
    if (write == 0 || cycle < write) {
        return mAddress + index;
    }
    return mAddress + (index / write) * cycle + (index % write);
}

u32 Vif::cycleOf(u32 index) const {
    const u32 write = (mCycle >> 8) & 0xFFu;
    const u32 position = write == 0 ? index : index % write;
    return position < kCycleLimit ? position : kCycleLimit - 1;
}

// A long cycle can step past the end of VU memory, which wraps rather than
// dropping the write.
u32 Vif::offsetBy(u32 lane, u32 value) {
    switch (mMode) {
        case kOffsetMode: return value + mRow[lane];
        case kDifferenceMode: return mRow[lane] += value;
        case kRowMode: return mRow[lane] = value;
        default: return value;
    }
}

// A quadword whose every lane comes from the row or column registers needs no
// data word, so the unit writes it without waiting for one.
bool Vif::needsData(u32 index) const {
    if (!mMasked) {
        return true;
    }
    const u32 cycle = cycleOf(index);
    for (u32 lane = 0; lane < lanesOf(mFormat); ++lane) {
        if (((mMask >> (cycle * 8u + lane * 2u)) & 3u) == 0) {
            return true;
        }
    }
    return false;
}

void Vif::drainMaskedQuadwords() {
    while (mRemaining != 0 && !needsData(mWritten)) {
        for (u32& element : mElement) {
            element = 0;
        }
        flushQuadword();
    }
}

void Vif::flushQuadword() {
    for (u32 lane = 0; lane < 4; ++lane) {
        writeMasked(mWritten, lane, mElement[lane]);
    }
    mLane = 0;
    ++mWritten;
    --mRemaining;
}

void Vif::writeLane(u32 quadword, u32 lane, u32 value) {
    const u32 offset = (quadword % (memorySize() / 16u)) * 16u + lane * 4u;
    mMemory->write32(memoryBase() + offset, value);
}

void Vif::writeMasked(u32 index, u32 lane, u32 value) {
    if (!mMasked) {
        writeLane(destinationOf(index), lane, offsetBy(lane, value));
        return;
    }
    const u32 cycle = cycleOf(index);
    switch ((mMask >> (cycle * 8u + lane * 2u)) & 3u) {
        case 0: writeLane(destinationOf(index), lane, offsetBy(lane, value)); return;
        case 1: writeLane(destinationOf(index), lane, mRow[lane]); return;
        case 2: writeLane(destinationOf(index), lane, mColumn[cycle]); return;
        default: return;
    }
}

void Vif::unpack(u32 word) {
    const u32 lanes = lanesOf(mFormat);
    const u32 width = widthOf(mFormat);
    const u32 perWord = width == 32 ? 1u : (32u / width);

    for (u32 i = 0; i < perWord && mRemaining != 0; ++i) {
        u32 value = word;
        if (width == 16) {
            const u32 half = (word >> (16 * i)) & 0xFFFFu;
            value = mUnsigned ? half : static_cast<u32>(static_cast<s32>(half << 16) >> 16);
        } else if (width == 8) {
            const u32 byte = (word >> (8 * i)) & 0xFFu;
            value = mUnsigned ? byte : static_cast<u32>(static_cast<s32>(byte << 24) >> 24);
        }

        if (mFormat == kUnpackFive) {
            const u32 pixel = word >> (16 * i);
            mElement[0] = (pixel & 0x1Fu) << 3;
            mElement[1] = ((pixel >> 5) & 0x1Fu) << 3;
            mElement[2] = ((pixel >> 10) & 0x1Fu) << 3;
            mElement[3] = ((pixel >> 15) & 1u) << 7;
            flushQuadword();
            drainMaskedQuadwords();
            continue;
        }
        mElement[mLane] = value;
        if (++mLane == lanes) {
            if (lanes == 1) {
                mElement[1] = mElement[2] = mElement[3] = value;
            } else if (lanes == 2) {
                mElement[2] = mElement[0];
                mElement[3] = mElement[1];
            } else if (lanes == 3) {
                mElement[3] = 0;
            }
            flushQuadword();
            drainMaskedQuadwords();
        }
    }
}

}  // namespace uee
