// SLUS_216.21 md5 9074ba25f5a100940ed233c9f8f675ab drives these.

#include "ipu.h"

#include <algorithm>

namespace uee {
namespace {

constexpr u32 kCommand = 0x00;
constexpr u32 kCommandHigh = 0x04;
constexpr u32 kControl = 0x10;
constexpr u32 kBitPosition = 0x20;
constexpr u32 kTop = 0x30;

constexpr u32 kControlWritable = 0x07F30000;
constexpr u32 kReset = 1u << 30;
// viBufStopDMA @ 0x5946a8 spins while CTRL reports busy, and viBufRestartDMA
// @ 0x5947a8 rewinds by what BP says the IPU still holds.
constexpr u32 kBusy = 1u << 31;
// What a decode leaves in the control register while it waits for a stream.
constexpr u32 kDecodeState = 0x3F00;

constexpr u32 kCommandShift = 28;
constexpr u32 kClearCommand = 0;
constexpr u32 kDecodeCommand = 1;
constexpr u32 kVariableCommand = 3;
constexpr u32 kQuantiserCommand = 5;
constexpr u32 kVectorCommand = 6;
constexpr u32 kThresholdCommand = 9;
constexpr u32 kPositionMask = 0x7F;
// A variable length read decodes against a picture state the unit was never
// given, so what it answers is what the rom left it able to say.
constexpr u32 kVariableResult = 0x000A0010;

// What the boot rom left in the unit, read before a program feeds it. The peek
// turns within a word rather than reading across the four.
constexpr u32 kLeftoverStream[4] = {0x8C31AD35, 0xFF7FCE39, 0x8C31AD35, 0xFF7FCE39};

constexpr u32 peekStream(u32 position) {
    const u32 word = kLeftoverStream[(position >> 5) & 3u];
    const u32 turn = position & 31u;
    return turn == 0 ? word : ((word << turn) | (word >> (32 - turn)));
}

}  // namespace

void Ipu::receive(std::span<const u8> bytes) {
    mControl &= ~kBusy;
    mStream.insert(mStream.end(), bytes.begin(), bytes.end());
}

void Ipu::consume(usize bytes) {
    const usize read = std::min(bytes, mStream.size());
    mStream.erase(mStream.begin(), mStream.begin() + static_cast<isize>(read));
}

std::optional<u32> Ipu::read(u32 address) const {
    switch (address) {
        case kControl: return mControl;
        case kBitPosition: return mBitPosition;
        case kTop: return peekStream(mBitPosition);
        // The command register answers with the bits under the position, and
        // its upper half with whether the unit is still working.
        case kCommand: return mDecoded ? mDecoded : peekStream(mBitPosition);
        case kCommandHigh: return mControl & kBusy;
        default: return std::nullopt;
    }
}

// A write that fell through to memory would leave CTRL reading back what was
// written, and viBufStopDMA spinning on it.
bool Ipu::write(u32 address, u32 value) {
    switch (address) {
        case kCommand:
            // Loading a table finishes on its own; decoding waits for input,
            // which is what a guest polling before it feeds the stream sees.
            mDecoded = 0;
            switch (value >> kCommandShift) {
                case kClearCommand: mBitPosition = value & kPositionMask; return true;
                case kThresholdCommand:
                case kQuantiserCommand:
                case kVectorCommand:
                    return true;
                case kDecodeCommand: mControl |= kBusy | kDecodeState; return true;
                case kVariableCommand:
                    mDecoded = kVariableResult;
                    mControl |= kBusy;
                    return true;
                default: mControl |= kBusy; return true;
            }
        case kControl:
            // Writing all ones and reading back is what this mask came from; the
            // rest of the register reports work this unit never starts.
            if ((value & kReset) != 0) {
                mControl = 0;
                mBitPosition = 0;
                mDecoded = 0;
                return true;
            }
            mControl = value & kControlWritable;
            return true;
        case kBitPosition:
        case kTop: return true;
        default: return false;
    }
}

}  // namespace uee
