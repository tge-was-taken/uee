#pragma once

#include "mmio.h"
#include "types.h"

#include <bit>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>

namespace uee {

constexpr u32 kMainRamSize = 32 * 1024 * 1024;
constexpr u32 kIopRamSize = 2 * 1024 * 1024;
constexpr u32 kIopHardwareBase = 0x1F800000;
constexpr u32 kIopHardwareSize = 0x00010000;
constexpr u32 kSpu2Base = 0x1F900000;
constexpr u32 kSpu2Size = 0x00001000;
constexpr u32 kScratchpadBase = 0x70000000;
constexpr u32 kScratchpadSize = 16 * 1024;
constexpr u32 kVu0MicroBase = 0x11000000;
constexpr u32 kVu0DataBase = 0x11004000;
constexpr u32 kVu0MemorySize = 4 * 1024;
constexpr u32 kVu1MicroBase = 0x11008000;
constexpr u32 kVu1DataBase = 0x1100C000;
constexpr u32 kVu1MemorySize = 16 * 1024;
constexpr u32 kUncachedBase = 0x20000000;
constexpr u32 kKernelBase = 0x80000000;
constexpr u32 kUnmappedBase = 0xC0000000;
// The uncached and uncached-accelerated windows mirror the low 256MB, so both
// drop the two bits that name the window; kseg0 and kseg1 keep the whole
// physical map, hardware registers included.
constexpr u32 kMirrorMask = 0x0FFFFFFF;
constexpr u32 kPhysicalMask = 0x1FFFFFFF;

struct Region {
    u32 base;
    std::vector<u8> bytes;
};

// kuseg, its uncached mirror, kseg0 and kseg1 are all windows onto the same
// physical memory. A default-constructed mirror folds nothing, so kseg2 upward
// is left alone and faults.
struct SegmentMirror {
    bool folds = false;

    constexpr u32 apply(u32 address) const {
        if (!folds) {
            return address;
        }
        if (address >= kUncachedBase && address < kKernelBase) {
            return address & kMirrorMask;
        }
        if (address >= kKernelBase && address < kUnmappedBase) {
            return address & kPhysicalMask;
        }
        return address;
    }
};

class Memory {
public:
    Memory(std::vector<Region> regions, SegmentMirror mirror);

    void attach(Mmio* mmio) {
        mMmio = mmio;
        mSegments.clear();
        if (mmio != nullptr) {
            const std::span<const MmioSegment> segments = mmio->segments();
            mSegments.assign(segments.begin(), segments.end());
        }
    }

    std::optional<u32> fault() const { return mFault; }

    std::optional<u8> peek8(u32 address) const {
        const u8* p = find(address, 1);
        if (p == nullptr) {
            p = find(mMirror.apply(address), 1);
        }
        return p == nullptr ? std::nullopt : std::optional<u8>{*p};
    }

    u8 read8(u32 address) {
        const u32 folded = mMirror.apply(address);
        if (mMmio != nullptr && isRegister(folded)) {
            if (const std::optional<u8> value = mMmio->readByte(folded)) {
                return *value;
            }
        }
        return load<u8>(resolve(address, 1));
    }
    u16 read16(u32 address) {
        const u32 folded = mMirror.apply(address);
        if (mMmio != nullptr && isRegister(folded)) {
            if (const std::optional<u16> value = mMmio->readHalf(folded)) {
                return *value;
            }
        }
        return load<u16>(resolve(address, 2));
    }

    // A register window only decodes a word at a time, so a doubleword access to
    // one is split.
    u64 read64(u32 address) {
        if (isMmio(address)) {
            return read32(address) | (static_cast<u64>(read32(address + 4)) << 32);
        }
        return load<u64>(resolve(address, 8));
    }

    u32 read32(u32 address) {
        const u32 folded = mMirror.apply(address);
        if (mMmio != nullptr && isRegister(folded)) {
            if (const std::optional<u32> value = mMmio->readWord(folded)) {
                return *value;
            }
        }
        return load<u32>(resolve(address, 4));
    }

    void write8(u32 address, u8 value) {
        const u32 folded = mMirror.apply(address);
        if (mMmio != nullptr && isRegister(folded) && mMmio->writeByte(folded, value)) {
            return;
        }
        store(resolve(address, 1), value);
    }
    void write16(u32 address, u16 value) {
        const u32 folded = mMirror.apply(address);
        if (mMmio != nullptr && isRegister(folded) && mMmio->writeHalf(folded, value)) {
            return;
        }
        store(resolve(address, 2), value);
    }
    void write64(u32 address, u64 value) {
        if (isMmio(address)) {
            write32(address, static_cast<u32>(value));
            write32(address + 4, static_cast<u32>(value >> 32));
            return;
        }
        store(resolve(address, 8), value);
    }

    void write32(u32 address, u32 value) {
        const u32 folded = mMirror.apply(address);
        if (mMmio != nullptr && isRegister(folded) &&
            mMmio->writeWord(folded, value)) {
            return;
        }
        store(resolve(address, 4), value);
    }

    // Speculative: must not latch a fault the guest never took.
    std::optional<u32> peek32(u32 address) {
        const u8* p = locate(address, 4);
        return p != nullptr ? std::optional(load<u32>(p)) : std::nullopt;
    }

private:

private:
    // Asking the register file costs a call that cannot inline.
    bool isRegister(u32 folded) const {
        for (const MmioSegment& segment : mSegments) {
            if (folded - segment.base < segment.size) {
                return true;
            }
        }
        return false;
    }

    bool isMmio(u32 address) const { return isRegister(mMirror.apply(address)); }

    template <typename T>
    static T load(const u8* p) {
        T value = 0;
        if (p != nullptr) {
            std::memcpy(&value, p, sizeof(T));
        }
        return value;
    }

    template <typename T>
    static void store(u8* p, T value) {
        if (p != nullptr) {
            std::memcpy(p, &value, sizeof(T));
        }
    }

    // The raw address decodes before the mirrored one, so the scratchpad still
    // reaches itself where the mask would fold it onto the hardware window.
    u8* locate(u32 address, u32 size) {
        if (u8* p = find(address, size)) {
            return p;
        }
        return find(mMirror.apply(address), size);
    }

    u8* resolve(u32 address, u32 size) {
        if (u8* p = locate(address, size)) {
            return p;
        }
        if (!mFault) {
            mFault = address;
        }
        return nullptr;
    }

    const u8* find(u32 address, u32 size) const {
        for (const Region& region : mRegions) {
            if (address < region.base) {
                continue;
            }
            const u32 offset = address - region.base;
            const u32 limit = static_cast<u32>(region.bytes.size());
            if (offset < limit && limit - offset >= size) {
                return region.bytes.data() + offset;
            }
        }
        return nullptr;
    }

    u8* find(u32 address, u32 size) {
        return const_cast<u8*>(std::as_const(*this).find(address, size));
    }

    std::vector<Region> mRegions;
    SegmentMirror mMirror;
    Mmio* mMmio = nullptr;
    std::vector<MmioSegment> mSegments;
    std::optional<u32> mFault;
};

Memory makeMainMemory();
Memory makeIopMemory();

}  // namespace uee
