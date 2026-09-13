#include "gif.h"

#include <algorithm>
#include <cstring>

#include "host.h"
#include "memory.h"

namespace uee {
namespace {

constexpr u32 kControlRegister = 0x10003000;
constexpr u32 kModeRegister = 0x10003010;
constexpr u32 kStatusRegister = 0x10003020;
constexpr u32 kPath3Mask = 1u << 0;
constexpr u32 kPath3Masked = 1u << 1;
constexpr u32 kReset = 1u << 0;
constexpr u32 kPaused = 1u << 3;
constexpr u32 kPath3 = 3;

constexpr u32 kFifoCountShift = 24;
constexpr u32 kActivePathShift = 10;

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
            // The first bit of the mode register is what the status reports as
            // the path 3 mask.
            return mStatus | (mMode & kPath3Mask) | (mActivePath << kActivePathShift) |
                   (std::min(static_cast<u32>(mHeld.size() / 2), kFifoDepth)
                    << kFifoCountShift);
        default: return std::nullopt;
    }
}

bool Gif::write(u32 address, u32 value) {
    switch (address) {
        case kControlRegister:
            mControl = value;
            if ((value & kReset) != 0) {
                mStatus = 0;
                mMode = 0;
                mActivePath = 0;
                mHeld.clear();
            }
            // The pause bit is the one part of the control register the status
            // register reports back.
            mStatus = (mStatus & ~kPaused) | (value & kPaused);
            return true;
        case kModeRegister: mMode = value; return true;
        default: return false;
    }
}

void Gif::beginPath(u32 path) {
    flushCapture();
    mActivePath = path;
}

void Gif::endPath() {
    flushCapture();
    mActivePath = 0;
    mStatus &= ~kOutputPathActive;
}

void Gif::capture(u64 low, u64 high) {
    if (mCapturedPath != mActivePath) {
        flushCapture();
        mCapturedPath = mActivePath;
    }
    const usize at = mCaptured.size();
    mCaptured.resize(at + 16);
    std::memcpy(mCaptured.data() + at, &low, sizeof(low));
    std::memcpy(mCaptured.data() + at + 8, &high, sizeof(high));
}

void Gif::flushCapture() {
    if (mHandler == nullptr || mCaptured.empty()) {
        return;
    }
    mHandler->transfer(mCapturedPath, mCaptured);
    mCaptured.clear();
}

// A masked path cannot reach the unit, so what it is sent waits in the queue.
// RenderWare starts a masked transfer and waits for the queue to fill.
void Gif::maskPath3(bool masked) {
    mStatus = masked ? (mStatus | kPath3Masked) : (mStatus & ~kPath3Masked);
    mPath3Masked = masked;
    if (!masked) {
        releaseHeldPacket();
    }
}

// The mask takes hold again at the end of a packet, and RenderWare's gate
// re-masks 96 NOPs after unmasking, so each unmask lets one packet through
// (SLUS_216.21, _rwDMADmaOpen 0x4d67b0 builds the gate).
void Gif::releaseHeldPacket() {
    if (mHeld.empty()) {
        return;
    }
    beginPath(kPath3);
    usize sent = 0;
    do {
        passQuadword(mHeld[sent], mHeld[sent + 1]);
        sent += 2;
    } while (sent < mHeld.size() && isPacketOpen());
    mHeld.erase(mHeld.begin(), mHeld.begin() + static_cast<std::ptrdiff_t>(sent));
    endPath();
}

// Nothing comes after the last unmask of a chain to stop the path, and
// RenderWare waits for the whole of it before kicking the next (SLUS_216.21,
// _rwDMAHandler 0x4d4350).
void Gif::drainPath3() {
    while (!mPath3Masked && !mHeld.empty()) {
        releaseHeldPacket();
    }
}

void Gif::readTag(u64 low, u64 high) {
    const u32 loops = static_cast<u32>(low) & 0x7FFFu;
    const u32 descriptors = static_cast<u32>(low >> 60) & 0xFu;
    Packet& open = getPacket();
    open.endOfPacket = ((low >> 15) & 1u) != 0;
    open.descriptors = descriptors == 0 ? 16u : descriptors;
    open.format = static_cast<u32>(low >> 58) & 3u;
    open.registers = high;
    open.remaining = quadwordsFor(open.format, loops, open.descriptors);
    open.slot = 0;
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
    if (mActivePath == kPath3 && (mPath3Masked || !mHeld.empty())) {
        mHeld.push_back(low);
        mHeld.push_back(high);
        return;
    }
    passQuadword(low, high);
}

void Gif::passQuadword(u64 low, u64 high) {
    if (mHandler != nullptr) {
        capture(low, high);
    }
    Packet& open = getPacket();
    if (open.remaining == 0) {
        readTag(low, high);
        return;
    }
    --open.remaining;
    if (open.format != kFormatPacked) {
        return;
    }
    const u32 descriptor = static_cast<u32>(open.registers >> (4 * open.slot)) & 0xFu;
    open.slot = (open.slot + 1) % open.descriptors;
    if (descriptor == kAddressData) {
        writeRegister(static_cast<u32>(high) & 0xFFu, low);
    }
}

}  // namespace uee
