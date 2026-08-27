#pragma once

#include "types.h"

#include <optional>
#include <vector>

namespace uee {

constexpr u32 kMainRamSize = 32 * 1024 * 1024;
constexpr u32 kScratchpadBase = 0x70000000;
constexpr u32 kScratchpadSize = 16 * 1024;
constexpr u32 kHardwareBase = 0x10000000;
constexpr u32 kHardwareSize = 0x00010000;
constexpr u32 kMirrorLimit = 0xC0000000;
constexpr u32 kPhysicalMask = 0x1FFFFFFF;

struct Region {
    u32 base;
    std::vector<u8> bytes;
};

// kuseg, its uncached mirror, kseg0 and kseg1 are all windows onto the same
// physical memory. A default-constructed mirror folds nothing, so kseg2 upward
// is left alone and faults.
struct SegmentMirror {
    u32 limit = 0;
    u32 mask = 0;

    constexpr u32 apply(u32 address) const {
        return address < limit ? (address & mask) : address;
    }
};

class Memory {
public:
    Memory(std::vector<Region> regions, SegmentMirror mirror);

    std::optional<u32> fault() const { return mFault; }

    u8 read8(u32 address);
    u16 read16(u32 address);
    u32 read32(u32 address);
    u64 read64(u32 address);

    void write8(u32 address, u8 value);
    void write16(u32 address, u16 value);
    void write32(u32 address, u32 value);
    void write64(u32 address, u64 value);

private:
    u8* resolve(u32 address, u32 size);
    u8* find(u32 address, u32 size);

    std::vector<Region> mRegions;
    SegmentMirror mMirror;
    std::optional<u32> mFault;
};

Memory makeMainMemory();

}  // namespace uee
