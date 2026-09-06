#include "gif.h"

#include "host.h"
#include "memory.h"

namespace uee {
namespace {

constexpr u32 kControlRegister = 0x10003000;
constexpr u32 kModeRegister = 0x10003010;
constexpr u32 kStatusRegister = 0x10003020;
constexpr u32 kPath3Masked = 1u << 1;

constexpr u32 kFifoCountShift = 24;
constexpr u32 kActivePathShift = 10;
// Nothing ever queues here, so a truthful zero leaves rwDMAHandler waiting for
// a transfer it has already been given.
constexpr u32 kFifoCount = 1;

constexpr u32 kSignalLabelId = 0x12001080;

constexpr u32 kSignal = 0x60;
constexpr u32 kFinish = 0x61;
constexpr u32 kLabel = 0x62;

constexpr u32 kAddressData = 0xE;
constexpr u32 kFormatPacked = 0;
constexpr u32 kFormatRegisterList = 1;

// A packed descriptor spends a whole quadword on one register, a register list
// packs two into one, and the image formats carry pixels with no list at all.
constexpr u32 quadwordsFor(u32 format, u32 loops, u32 descriptors) {
    switch (format) {
        case kFormatPacked: return loops * descriptors;
        case kFormatRegisterList: return (loops * descriptors + 1) / 2;
        default: return loops;
    }
}

constexpr u32 merge(u32 previous, u64 value) {
    const u32 bits = static_cast<u32>(value);
    const u32 mask = static_cast<u32>(value >> 32);
    return (previous & ~mask) | (bits & mask);
}

}  // namespace

std::optional<u32> Gif::read(u32 address) const {
    switch (address) {
        case kControlRegister: return mControl;
        case kModeRegister: return mMode;
        case kStatusRegister:
            return mStatus | (mActivePath << kActivePathShift) |
                   (kFifoCount << kFifoCountShift);
        default: return std::nullopt;
    }
}

bool Gif::write(u32 address, u32 value) {
    switch (address) {
        case kControlRegister: mControl = value; return true;
        case kModeRegister: mMode = value; return true;
        default: return false;
    }
}

void Gif::beginPath(u32 path) {
    flushCapture();
    mActivePath = path;
    mQueued = 0;
}

void Gif::endPath() {
    flushCapture();
    mActivePath = 0;
    mQueued = 0;
}

void Gif::capture(u64 low, u64 high) {
    if (mCapturedPath != mActivePath) {
        flushCapture();
        mCapturedPath = mActivePath;
    }
    for (u32 shift = 0; shift < 64; shift += 8) {
        mCaptured.push_back(static_cast<u8>(low >> shift));
    }
    for (u32 shift = 0; shift < 64; shift += 8) {
        mCaptured.push_back(static_cast<u8>(high >> shift));
    }
}

void Gif::flushCapture() {
    if (mHandler == nullptr || mCaptured.empty()) {
        return;
    }
    mHandler->transfer(mCapturedPath, mCaptured);
    mCaptured.clear();
}

void Gif::maskPath3(bool masked) {
    mStatus = masked ? (mStatus | kPath3Masked) : (mStatus & ~kPath3Masked);
}

void Gif::readTag(u64 low, u64 high) {
    const u32 loops = static_cast<u32>(low) & 0x7FFFu;
    mEndOfPacket = ((low >> 15) & 1u) != 0;
    const u32 descriptors = static_cast<u32>(low >> 60) & 0xFu;
    mDescriptors = descriptors == 0 ? 16u : descriptors;
    mFormat = static_cast<u32>(low >> 58) & 3u;
    mRegisters = high;
    mRemaining = quadwordsFor(mFormat, loops, mDescriptors);
    mSlot = 0;
}

void Gif::writeRegister(u32 address, u64 value) {
    const u64 identifiers = mMemory->read64(kSignalLabelId);
    switch (address) {
        case kSignal:
            mMemory->write64(kSignalLabelId,
                            (identifiers & 0xFFFFFFFF00000000ull) |
                                merge(static_cast<u32>(identifiers), value));
            mGs->raiseSignal();
            return;
        case kFinish:
            mGs->raiseFinish();
            return;
        case kLabel:
            mMemory->write64(kSignalLabelId,
                            static_cast<u32>(identifiers) |
                                (static_cast<u64>(merge(static_cast<u32>(identifiers >> 32),
                                                        value))
                                 << 32));
            return;
        default:
            return;
    }
}

void Gif::writeQuadword(u64 low, u64 high) {
    ++mQueued;
    if (mHandler != nullptr) {
        capture(low, high);
    }
    if (mRemaining == 0) {
        readTag(low, high);
        return;
    }
    --mRemaining;
    if (mFormat != kFormatPacked) {
        return;
    }
    const u32 descriptor = static_cast<u32>(mRegisters >> (4 * mSlot)) & 0xFu;
    mSlot = (mSlot + 1) % mDescriptors;
    if (descriptor == kAddressData) {
        writeRegister(static_cast<u32>(high) & 0xFFu, low);
    }
}

}  // namespace uee
