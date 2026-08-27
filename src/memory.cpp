#include "memory.h"

#include "hardware.h"

#include <bit>
#include <cstring>
#include <utility>

static_assert(std::endian::native == std::endian::little,
              "guest memory is copied verbatim, so the host must be little-endian");

namespace uee {
namespace {

template <typename T>
T load(const u8* p) {
    T value = 0;
    if (p != nullptr) {
        std::memcpy(&value, p, sizeof(T));
    }
    return value;
}

template <typename T>
void store(u8* p, T value) {
    if (p != nullptr) {
        std::memcpy(p, &value, sizeof(T));
    }
}

}  // namespace

Memory::Memory(std::vector<Region> regions, SegmentMirror mirror)
    : mRegions(std::move(regions)), mMirror(mirror) {}

u8* Memory::find(u32 address, u32 size) {
    for (Region& region : mRegions) {
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

// The raw address decodes before the mirrored one, so the scratchpad still
// reaches itself where the mask would fold it onto the hardware window.
u8* Memory::resolve(u32 address, u32 size) {
    u8* p = find(address, size);
    if (p == nullptr) {
        p = find(mMirror.apply(address), size);
    }
    if (p == nullptr && !mFault) {
        mFault = address;
    }
    return p;
}

u8 Memory::read8(u32 address) { return load<u8>(resolve(address, 1)); }
u16 Memory::read16(u32 address) { return load<u16>(resolve(address, 2)); }
u32 Memory::read32(u32 address) {
    if (mHardware != nullptr) {
        if (const std::optional<u32> value = mHardware->read(mMirror.apply(address))) {
            return *value;
        }
    }
    return load<u32>(resolve(address, 4));
}
u64 Memory::read64(u32 address) { return load<u64>(resolve(address, 8)); }

void Memory::write8(u32 address, u8 value) { store(resolve(address, 1), value); }
void Memory::write16(u32 address, u16 value) { store(resolve(address, 2), value); }
void Memory::write32(u32 address, u32 value) {
    if (mHardware != nullptr && mHardware->write(mMirror.apply(address), value)) {
        return;
    }
    store(resolve(address, 4), value);
}
void Memory::write64(u32 address, u64 value) { store(resolve(address, 8), value); }

Memory makeMainMemory() {
    std::vector<Region> regions;
    regions.push_back({0, std::vector<u8>(kMainRamSize)});
    regions.push_back({kHardwareBase, std::vector<u8>(kHardwareSize)});
    regions.push_back({kScratchpadBase, std::vector<u8>(kScratchpadSize)});
    regions.push_back({kVu0MicroBase, std::vector<u8>(kVu0MemorySize)});
    regions.push_back({kVu0DataBase, std::vector<u8>(kVu0MemorySize)});
    regions.push_back({kGsPrivBase, std::vector<u8>(kGsPrivSize)});
    return Memory{std::move(regions), {kMirrorLimit, kPhysicalMask}};
}

}  // namespace uee
