#pragma once

#include "compiler.h"
#include "mmio.h"
#include "types.h"

#include <bit>
#include <cassert>
#include <cstring>
#include <span>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace uee {

// Odd, so a resumed function cannot mistake it for a return address.
constexpr u32 kResumedNotCalled = 1;

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
// The uncached windows mirror the low 256MB; kseg0 and kseg1 keep the whole map.
constexpr u32 kMirrorMask = 0x0FFFFFFF;
constexpr u32 kPhysicalMask = 0x1FFFFFFF;

struct FixedAddress {
    u32 address;
};

struct Region {
    u32 base;
    std::vector<u8> bytes;
};

// A default-constructed mirror folds nothing, so kseg2 upward faults.
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

enum class Window {
    Ram,
    Mmio,
    Elsewhere,
};

struct Resolved {
    Window window = Window::Elsewhere;
    u32 address = 0;
};

constexpr Resolved resolveWindow(u32 address) {
    // Raw before folded, so a window reaching itself is not taken for the mask's.
    if (address < kMainRamSize) {
        return {Window::Ram, address};
    }
    if (address - kScratchpadBase < kScratchpadSize) {
        return {Window::Elsewhere, address};
    }
    const u32 folded = SegmentMirror{true}.apply(address);
    if (folded - kMmioBase < kMmioSize || folded - kGsPrivBase < kGsPrivSize) {
        return {Window::Mmio, folded};
    }
    if (folded < kMainRamSize) {
        return {Window::Ram, folded};
    }
    return {Window::Elsewhere, address};
}

class Memory {
public:
    Memory(std::vector<Region> regions, SegmentMirror mirror);

    // The cached window points into the first region.
    Memory(const Memory&) = delete;
    Memory& operator=(const Memory&) = delete;
    Memory(Memory&&) = default;

    Mmio& mmio() const {
        if (mMmio == nullptr) {
            throw std::runtime_error("this memory has no devices attached");
        }
        return *mMmio;
    }

    void attach(Mmio* mmio) {
        mMmio = mmio;
        mSegments.clear();
        if (mmio != nullptr) {
            const std::span<const MmioSegment> segments = mmio->segments();
            mSegments.assign(segments.begin(), segments.end());
        }
    }

    // Reported once, so the next access is judged on its own.
    std::optional<u32> takeFault() { return std::exchange(mFault, std::nullopt); }

    // Empty when no region starts there.
    std::span<u8> regionAt(u32 base) {
        for (Region& candidate : mRegions) {
            if (candidate.base == base) {
                return candidate.bytes;
            }
        }
        return {};
    }

    u8 readRam8(u32 address) const { return *ram(address); }
    u16 readRam16(u32 address) const { return loadAt<u16>(ram(address)); }
    u32 readRam32(u32 address) const { return loadAt<u32>(ram(address)); }
    u64 readRam64(u32 address) const { return loadAt<u64>(ram(address)); }

    void writeRam8(u32 address, u8 value) { *ram(address) = value; }
    void writeRam16(u32 address, u16 value) { storeAt(ram(address), value); }
    void writeRam32(u32 address, u32 value) { storeAt(ram(address), value); }
    void writeRam64(u32 address, u64 value) { storeAt(ram(address), value); }

    // The offsets no device claims. Only ee_mmio.h's block dispatch reaches these.
    u32 readMmioBacking32(u32 folded);
    void writeMmioBacking32(u32 folded, u32 value);

    std::optional<u8> peek8(u32 address) const {
        const u8* p = find(address, 1);
        if (p == nullptr) {
            p = find(mMirror.apply(address), 1);
        }
        return p == nullptr ? std::nullopt : std::optional<u8>{*p};
    }

    // Main memory inline and the rest out of line, so an access costs a compare.
    u8 read8(u32 address) {
        return inRam(address) ? readRam8(address) : readVirtual8(address);
    }
    u16 read16(u32 address) {
        return inRam(address) ? readRam16(address) : readVirtual16(address);
    }

    // A register window decodes a word at a time, so a doubleword splits.
    u64 read64(u32 address) {
        return inRam(address) ? readRam64(address) : readVirtual64(address);
    }

    u32 read32(u32 address) {
        return inRam(address) ? readRam32(address) : readVirtual32(address);
    }

    void write8(u32 address, u8 value) {
        if (inRam(address)) {
            writeRam8(address, value);
            return;
        }
        writeVirtual8(address, value);
    }
    void write16(u32 address, u16 value) {
        if (inRam(address)) {
            writeRam16(address, value);
            return;
        }
        writeVirtual16(address, value);
    }
    void write64(u32 address, u64 value) {
        if (inRam(address)) {
            writeRam64(address, value);
            return;
        }
        writeVirtual64(address, value);
    }

    void write32(u32 address, u32 value) {
        if (inRam(address)) {
            writeRam32(address, value);
            return;
        }
        writeVirtual32(address, value);
    }

    // Speculative: must not latch a fault the guest never took.
    std::optional<u32> peek32(u32 address) {
        const u8* p = locate(address, 4);
        return p != nullptr ? std::optional(load<u32>(p)) : std::nullopt;
    }

    // No register window sits below main memory, so one comparison answers the
    // mirror and the register check together.
    bool inRam(u32 address) const { return address < mRamFast; }

    // The scratchpad folds onto main memory, so it is never a register window.
    u8* scratchpadAt(u32 address, u32 size) const {
        const u32 inPad = address - kScratchpadBase;
        const bool inside = inPad < kScratchpadSize && inPad + size <= kScratchpadSize;
        return inside ? mScratchpadBytes + inPad : nullptr;
    }

private:
    // Out of line: no guest reaches these often enough to pay for the room.
    UEE_COLD u8 readVirtual8(u32 address);
    UEE_COLD u16 readVirtual16(u32 address);
    UEE_COLD u32 readVirtual32(u32 address);
    UEE_COLD u64 readVirtual64(u32 address);
    UEE_COLD void writeVirtual8(u32 address, u8 value);
    UEE_COLD void writeVirtual16(u32 address, u16 value);
    UEE_COLD void writeVirtual32(u32 address, u32 value);
    UEE_COLD void writeVirtual64(u32 address, u64 value);

    u8* ram(u32 address) const {
        assert(mRamBytes != nullptr && address < mRamLimit);
        return mRamBytes + address;
    }

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

    // No null case inside a window, so none is asked about.
    template <typename T>
    static T loadAt(const u8* p) {
        T value;
        std::memcpy(&value, p, sizeof(T));
        return value;
    }

    template <typename T>
    static void storeAt(u8* p, T value) {
        std::memcpy(p, &value, sizeof(T));
    }

    // Raw before mirrored, or the mask folds the scratchpad onto the hardware
    // window.
    u8* locate(u32 address, u32 size) {
        const u32 inPad = address - kScratchpadBase;
        if (inPad < kScratchpadSize && inPad + size <= kScratchpadSize) {
            return mScratchpadBytes + inPad;
        }
        if (u8* p = find(address, size)) {
            return p;
        }
        return find(mMirror.apply(address), size);
    }

    u8* resolveAddress(u32 address, u32 size) {
        if (u8* p = locate(address, size)) {
            return p;
        }
        if (!mFault) {
            mFault = address;
        }
        return nullptr;
    }

    // Spans kept beside the regions, so choosing one chases no pointers.
    struct Extent {
        u32 base = 0;
        u32 size = 0;
    };

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
    std::vector<Extent> mExtents;
    u8* mRamBytes = nullptr;
    u8* mScratchpadBytes = nullptr;
    // Below any real size, so an empty window rejects every address.
    u32 mRamLimit = 0;
    u32 mRamFast = 0;
    SegmentMirror mMirror;
    Mmio* mMmio = nullptr;
    std::vector<MmioSegment> mSegments;
    std::optional<u32> mFault;
};

Memory makeMainMemory();
Memory makeIopMemory();

}  // namespace uee
