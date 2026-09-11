#include "vif.h"

#include "log.h"

#include <algorithm>
#include <format>
#include <stdexcept>

#include "gif.h"
#include "memory.h"
#include "vu_micro.h"

namespace uee {
namespace {

LoggerChannel log("vif");

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
// These carry data of their own, and a count of zero means the field's
// maximum rather than nothing.
constexpr u32 kMpg = 0x4A, kDirect = 0x50, kDirectHl = 0x51;
constexpr u32 kFlushe = 0x10, kFlush = 0x11, kFlusha = 0x13;
constexpr u32 kMscal = 0x14, kMscalf = 0x15, kMscnt = 0x17;
}  // namespace code

constexpr u32 kMicroWordsMax = 256 * 2;
constexpr u32 kDirectQuadwordsMax = 0x10000;

constexpr u32 kDataWait = 1;
constexpr u32 kDecoding = 2;
constexpr u32 kProgramWait = 1u << 2;
constexpr u32 kFifoCountShift = 24;
constexpr u32 kMarkFlag = 1u << 6;
constexpr u32 kInterruptStall = 1u << 10;
constexpr u32 kInterruptFlag = 1u << 11;
constexpr u32 kUnknownCode = 1u << 13;

constexpr u32 kMaskInterrupt = 1u << 0;
constexpr u32 kMaskCodeError = 1u << 2;
constexpr u32 kErrorBits = 7u;
constexpr u32 kTransferring = 3u;
constexpr u32 kDoubleBuffer = 1u << 7;
constexpr u32 kStopStall = 1u << 8;
constexpr u32 kForceBreakStall = 1u << 9;

constexpr u32 kForceBreak = 1u << 1;
constexpr u32 kStop = 1u << 2;
constexpr u32 kStallCancel = 1u << 3;

constexpr u32 kCodeInterrupt = 1u << 31;

constexpr u32 kUnpack = 0x60;
constexpr u32 kUnpackMask = 0x60;
constexpr u32 kUnpackMasked = 0x10;
constexpr u32 kUnpackFive = 0xF;

constexpr u32 kOffsetMode = 1;
constexpr u32 kDifferenceMode = 2;
constexpr u32 kRowMode = 3;

constexpr u32 kCycleLimit = 4;

constexpr u32 kPath2 = 2;

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
u32 Vif::memoryBase() const { return mUnit == 0 ? kVu0DataBase : kVu1DataBase; }
u32 Vif::memorySize() const { return mUnit == 0 ? kVu0MemorySize : kVu1MemorySize; }
u32 Vif::microBase() const { return mUnit == 0 ? kVu0MicroBase : kVu1MicroBase; }
u32 Vif::microSize() const { return mUnit == 0 ? kVu0MemorySize : kVu1MemorySize; }

std::optional<u32> Vif::read(u32 address) {
    if (address < registerBase() || address >= registerBase() + kRegisterSpan) {
        return std::nullopt;
    }
    switch (address - registerBase()) {
        case kStat: {
            // Nothing waited for a quadword the processor wrote, so the unit is
            // still decoding it the first time the guest looks and settled by
            // the next.
            const u32 decoding = mFedByProcessor ? kDecoding : 0u;
            mFedByProcessor = false;
            return (mStatus & ~3u) | (fifoCount() << kFifoCountShift) | decoding |
                   ((mUnit == 1 && mSecondBuffer) ? kDoubleBuffer : 0u) |
                   (mStalled ? kDecoding
                             : (isTransferring() ? kTransferring
                                                 : (wantsData() ? kDataWait : 0u)));
        }
        case kErr: return mError;
        case kMark: return mMark;
        case kCycle: return mCycle;
        case kMode: return mMode;
        // The first unit's count is eight bits wide, so a full 256 reads back
        // as zero there; the second unit has room for it.
        // A latch rather than the live counter: a reset ends the transfer but
        // leaves this reading what it did.
        case kNum: return mUnit == 0 ? (mNum & 0xFFu) : mNum;
        case kMask: return mMask;
        case kCode: return mCode;
        case kItops: return mItops;
        case kBase: return mBase;
        case kOffset: return mOffset;
        case kTops: return mTops;
        case kItop: return mItop;
        case kTop: return mTop;
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
        // Nothing waits for a quadword the processor writes the way the
        // controller waits for one it sends, so the unit is still decoding it.
        mFedByProcessor = true;
        if (mStalled) {
            ++mFifoWords;
            return true;
        }
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
            // The unit reports the stall at once but only stops where a code
            // boundary lets it, so it is not decoding yet.
            if (value & kForceBreak) {
                mStatus |= kForceBreakStall;
                mStallRequested = true;
            }
            if (value & kStop) {
                mStatus |= kStopStall;
                mStallRequested = true;
            }
            if (value & kStallCancel) {
                mStatus &= ~(kForceBreakStall | kStopStall | kInterruptStall);
                mStalled = false;
                mStallRequested = false;
                mFifoWords = 0;
            }
            if (value & 1u) {
                mStatus = 0;
                mSecondBuffer = false;
                mStalled = false;
                mStallRequested = false;
                mRemaining = 0;
                mPending = 0;
                mPendingTarget = nullptr;
                mMicroWords = 0;
                mDirectWords = 0;
                mDirectFill = 0;
                mHoldingThree = false;
                mFifoWords = 0;
                mFedByProcessor = false;
            }
            return true;
        case kErr: mError = value & kErrorBits; return true;
        case kMark: mMark = value; mStatus &= ~kMarkFlag; return true;
        default: return true;
    }
}

// The first unit holds half what the second does, and the count moves in the
// halves the write port takes a quadword in.
u32 Vif::fifoCount() const {
    return std::min((mFifoWords / 4) * 2, mUnit == 0 ? 8u : 16u);
}

void Vif::writeTag(u64 codes) {
    mFedByProcessor = false;
    consume(static_cast<u32>(codes));
    consume(static_cast<u32>(codes >> 32));
}

void Vif::writeQuadword(u64 low, u64 high) {
    mFedByProcessor = false;
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
    if (mDirectWords != 0) {
        sendToGif(word);
        return;
    }
    if (mMicroWords != 0) {
        uploadMicro(word);
        return;
    }
    if (mPendingTarget != nullptr) {
        *mPendingTarget++ = word;
        if (--mPending == 0) {
            mPendingTarget = nullptr;
        }
        mNum = mPending;
    } else if (mRemaining != 0) {
        if (!unpack(word)) {
            execute(word);
        }
    } else {
        execute(word);
    }
    if (mStallRequested && mPendingTarget == nullptr && mRemaining == 0) {
        log.warn("unit {} stalled on {:08x}", mUnit, mCode);
        mStalled = true;
    }
}

void Vif::uploadMicro(u32 word) {
    mMemory->write32(microBase() + (mMicroWord % (microSize() / 4u)) * 4u, word);
    ++mMicroWord;
    --mMicroWords;
    mNum = (mMicroWords + 1) / 2;
}

Vu& Vif::vu() const {
    if (mVu == nullptr) {
        throw std::runtime_error(
            std::format("vif{} was asked for a vector unit it was not given", mUnit));
    }
    return *mVu;
}

// The buffer the program is about to read is the one the unpacks before it
// filled, and the next unpacks fill the other half.
void Vif::startMicro(u32 start) {
    mTop = mTops;
    mItop = mItops;
    mSecondBuffer = !mSecondBuffer;
    mTops = (mBase + (mSecondBuffer ? mOffset : 0)) & 0x3FFu;
    vu().microRunning = !runMicroProgram(vu(), *mMemory, mGif, mUnit, start);
}

void Vif::sendToGif(u32 word) {
    mDirectQuadword[mDirectFill++] = word;
    --mDirectWords;
    mNum = (mDirectWords + 3) / 4;
    if (mDirectFill < 4) {
        return;
    }
    mDirectFill = 0;
    if (mGif != nullptr) {
        mGif->writeQuadword(static_cast<u64>(mDirectQuadword[0]) |
                                (static_cast<u64>(mDirectQuadword[1]) << 32),
                            static_cast<u64>(mDirectQuadword[2]) |
                                (static_cast<u64>(mDirectQuadword[3]) << 32));
        if (mDirectWords == 0) {
            mGif->endPath();
        }
    }
}

// A code that takes its operand from the words behind it leaves the unit
// waiting the same way an unpack does.
bool Vif::wantsData() const { return mRemaining != 0 || mPending != 0; }

// A block of microcode or gif data goes through the unit rather than into its
// own registers, which it reports as a transfer rather than a wait.
bool Vif::isTransferring() const { return mMicroWords != 0 || mDirectWords != 0; }

// The second unit has the codes that reach the gif and the double buffer; the
// first stops on those the way it does on any code it cannot decode.
bool Vif::decodes(u32 command) const {
    switch (command) {
        case code::kOffset:
        case code::kBase:
        case code::kMskpath3:
        case code::kFlush:
        case code::kFlusha:
        case code::kMscalf:
        case code::kDirect:
        case code::kDirectHl:
            return mUnit == 1;
        case code::kNop:
        case code::kStcycl:
        case code::kItop:
        case code::kStmod:
        case code::kMark:
        case code::kFlushe:
        case code::kMscal:
        case code::kMscnt:
        case code::kStmask:
        case code::kStrow:
        case code::kStcol:
        case code::kMpg:
            return true;
        default:
            return (command & kUnpackMask) == kUnpack;
    }
}

namespace {

bool startsMicro(u32 command) {
    return command == code::kMscal || command == code::kMscalf ||
           command == code::kMscnt;
}

}  // namespace

void Vif::execute(u32 code) {
    mCode = code;
    const u32 command = (code >> 24) & 0x7Fu;
    const u32 immediate = code & 0xFFFFu;

    if (!decodes(command)) {
        if ((mError & kMaskCodeError) == 0) {
            mStatus |= kUnknownCode;
            mStallRequested = true;
        }
        return;
    }

    // The count reports what the code being executed still owes, so one that
    // carries no data of its own leaves nothing outstanding.
    mNum = 0;

    // A program that never reached its end bit is still running, and the unit
    // waits there rather than starting a second one over it.
    if (vu().microRunning && startsMicro(command)) {
        mStatus |= kProgramWait;
        mStalled = true;
        return;
    }

    // A code that asks for an interrupt raises one and stops the unit until the
    // guest resets it. Mark is the exception: it reports itself and carries on.
    if ((code & kCodeInterrupt) != 0 && command != code::kMark &&
        (mError & kMaskInterrupt) == 0) {
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
        // The offset is what restarts double buffering, from the first buffer;
        // a base on its own waits for the next swap or offset to take effect.
        case code::kOffset:
            mOffset = immediate & 0x3FFu;
            mTops = mBase;
            mSecondBuffer = false;
            return;
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
        case code::kMpg: {
            const u32 count = (code >> 16) & 0xFFu;
            mMicroWords = (count == 0 ? kMicroWordsMax : count * 2);
            mMicroWord = immediate * 2;
            mNum = mMicroWords / 2;
            return;
        }
        case code::kFlushe:
        case code::kFlush:
        case code::kFlusha:
            return;
        case code::kMscal:
        case code::kMscalf:
            startMicro(immediate);
            return;
        case code::kMscnt:
            startMicro(vu().vi[Vu::kTpc]);
            return;
        case code::kDirect:
        case code::kDirectHl:
            mDirectWords = (immediate == 0 ? kDirectQuadwordsMax : immediate) * 4;
            mNum = mDirectWords / 4;
            mGif->beginPath(kPath2);
            return;
        case code::kStmask: mPending = 1; mNum = 1; mPendingTarget = &mMask; return;
        case code::kStrow: mPending = 4; mNum = 4; mPendingTarget = mRow; return;
        case code::kStcol: mPending = 4; mNum = 4; mPendingTarget = mColumn; return;
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
        mAddress += mTops;
    }
    mRemaining = count == 0 ? 256u : count;
    mNum = mRemaining;
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

// A zero write cycle is the widest one rather than none at all.
u32 Vif::writeCycle() const {
    const u32 write = (mCycle >> 8) & 0xFFu;
    return write == 0 ? 256u : write;
}

// A write cycle wider than the read cycle leaves quadwords the data does not
// reach, which the unit fills from the last element it wrote rather than
// reading more.
bool Vif::isFillCycle(u32 index) const {
    const u32 read = mCycle & 0xFFu;
    return read < writeCycle() && (index % writeCycle()) >= read;
}

u32 Vif::cycleOf(u32 index) const {
    const u32 position = index % writeCycle();
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
    while (mRemaining != 0 && (isFillCycle(mWritten) || !needsData(mWritten))) {
        if (isFillCycle(mWritten)) {
            mElement[0] = mElement[2] = mFill[0];
            mElement[1] = mElement[3] = mFill[1];
        } else {
            for (u32& element : mElement) {
                element = 0;
            }
        }
        flushQuadword();
    }
}

void Vif::flushQuadword() {
    if (!isFillCycle(mWritten)) {
        mFill[0] = mElement[0];
        mFill[1] = mElement[1];
    }
    for (u32 lane = 0; lane < 4; ++lane) {
        writeMasked(mWritten, lane, mElement[lane]);
    }
    mLane = 0;
    ++mWritten;
    --mRemaining;
    mNum = mRemaining;
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

bool Vif::unpack(u32 word) {
    const u32 lanes = lanesOf(mFormat);
    const u32 width = widthOf(mFormat);
    const u32 perWord = width == 32 ? 1u : (32u / width);

    for (u32 i = 0; i < perWord && (mRemaining != 0 || mHoldingThree); ++i) {
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
        // A three lane element takes its fourth lane from the value that starts
        // the next one, so the write waits until that value arrives. After the
        // last element that value is only looked at: a word it starts belongs to
        // whatever follows the unpack.
        if (mHoldingThree) {
            mHoldingThree = false;
            mElement[3] = value;
            flushQuadword();
            drainMaskedQuadwords();
            if (mRemaining == 0) {
                mLane = 0;
                if (i == 0) {
                    return false;
                }
                continue;
            }
            mElement[0] = value;
            mElement[1] = 0;
            mElement[2] = 0;
            mLane = 1;
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
                mHoldingThree = true;
                mLane = 0;
                continue;
            }
            flushQuadword();
            drainMaskedQuadwords();
        }
    }
    return true;
}

}  // namespace uee
