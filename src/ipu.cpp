// SLUS_216.21 md5 9074ba25f5a100940ed233c9f8f675ab drives these.

#include "ipu.h"

#include <algorithm>

namespace uee {
namespace {

constexpr u32 kCommand = 0x00;
constexpr u32 kControl = 0x10;
constexpr u32 kBitPosition = 0x20;
constexpr u32 kTop = 0x30;

// viBufStopDMA @ 0x5946a8 spins while CTRL reports busy, and viBufRestartDMA
// @ 0x5947a8 rewinds by what BP says the IPU still holds.
constexpr u32 kIdle = 0;

}  // namespace

void Ipu::receive(std::span<const u8> bytes) {
    mStream.insert(mStream.end(), bytes.begin(), bytes.end());
}

void Ipu::consume(usize bytes) {
    const usize read = std::min(bytes, mStream.size());
    mStream.erase(mStream.begin(), mStream.begin() + static_cast<isize>(read));
}

std::optional<u32> Ipu::read(u32 address) const {
    switch (address) {
        case kCommand:
        case kControl:
        case kBitPosition:
        case kTop: return kIdle;
        default: return std::nullopt;
    }
}

// A write that fell through to memory would leave CTRL reading back what was
// written, and viBufStopDMA spinning on it.
bool Ipu::write(u32 address, u32) {
    switch (address) {
        case kCommand:
        case kControl:
        case kBitPosition:
        case kTop: return true;
        default: return false;
    }
}

}  // namespace uee
