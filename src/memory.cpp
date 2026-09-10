#include "memory.h"

#include <bit>
#include <utility>

static_assert(std::endian::native == std::endian::little,
              "guest memory is copied verbatim, so the host must be little-endian");

namespace uee {
namespace {

constexpr u32 kWidestAccess = 8;

}  // namespace


// find answers with the first region that covers an address, so a region based
// at zero is the one every low address reaches.
Memory::Memory(std::vector<Region> regions, SegmentMirror mirror)
    : mRegions(std::move(regions)), mMirror(mirror) {
    mExtents.reserve(mRegions.size());
    for (const Region& region : mRegions) {
        mExtents.push_back(
            {region.base, static_cast<u32>(region.bytes.size()) * region.repeats});
    }
    if (!mRegions.empty() && mRegions.front().base == 0) {
        mRamBytes = mRegions.front().bytes.data();
        mRamLimit = static_cast<u32>(mRegions.front().bytes.size());
        mRamFast = mRamLimit < kWidestAccess ? 0 : mRamLimit - kWidestAccess;
    }
    for (Region& region : mRegions) {
        if (region.base == kScratchpadBase && region.bytes.size() >= kScratchpadSize) {
            mScratchpadBytes = region.bytes.data();
        }
    }
}

UEE_COLD u32 Memory::readMmioBacking32(u32 folded) {
    return load<u32>(resolveAddress(folded, 4));
}

UEE_COLD void Memory::writeMmioBacking32(u32 folded, u32 value) {
    store(resolveAddress(folded, 4), value);
}

u8 Memory::readVirtual8(u32 address) {
    if (const u8* pad = scratchpadAt(address, 1)) {
        return loadAt<u8>(pad);
    }
    const u32 folded = mMirror.apply(address);
    if (mMmio != nullptr && isRegister(folded)) {
        if (const std::optional<u8> value = mMmio->readByte(folded)) {
            return *value;
        }
    }
    return load<u8>(resolveAddress(address, 1));
}

u16 Memory::readVirtual16(u32 address) {
    if (const u8* pad = scratchpadAt(address, 2)) {
        return loadAt<u16>(pad);
    }
    const u32 folded = mMirror.apply(address);
    if (mMmio != nullptr && isRegister(folded)) {
        if (const std::optional<u16> value = mMmio->readHalf(folded)) {
            return *value;
        }
    }
    return load<u16>(resolveAddress(address, 2));
}

u32 Memory::readVirtual32(u32 address) {
    if (const u8* pad = scratchpadAt(address, 4)) {
        return loadAt<u32>(pad);
    }
    const u32 folded = mMirror.apply(address);
    if (mMmio != nullptr && isRegister(folded)) {
        if (const std::optional<u32> value = mMmio->readWord(folded)) {
            return *value;
        }
    }
    return load<u32>(resolveAddress(address, 4));
}

// A register window only decodes a word at a time, so a doubleword access to
// one is split.
u64 Memory::readVirtual64(u32 address) {
    if (const u8* pad = scratchpadAt(address, 8)) {
        return loadAt<u64>(pad);
    }
    const u32 folded = mMirror.apply(address);
    if (mMmio != nullptr && isRegister(folded)) {
        if (const std::optional<u64> value = mMmio->readDouble(folded)) {
            return *value;
        }
        return read32(address) | (static_cast<u64>(read32(address + 4)) << 32);
    }
    return load<u64>(resolveAddress(address, 8));
}

void Memory::writeVirtual8(u32 address, u8 value) {
    if (u8* pad = scratchpadAt(address, 1)) {
        storeAt(pad, value);
        return;
    }
    if (isVuWindow(address)) {
        widenToWord(address, value, 0xFFu);
        return;
    }
    const u32 folded = mMirror.apply(address);
    if (mMmio != nullptr && isRegister(folded) && mMmio->writeByte(folded, value)) {
        return;
    }
    store(resolveAddress(address, 1), value);
}

// The rest of the word the unit could not have left alone reads as ones.
void Memory::widenToWord(u32 address, u32 value, u32 mask) {
    const u32 shift = (address & 3u) * 8u;
    const u32 kept = ~(mask << shift);
    writeVirtual32(address & ~3u, kept | ((value & mask) << shift));
}

void Memory::writeVirtual16(u32 address, u16 value) {
    if (u8* pad = scratchpadAt(address, 2)) {
        storeAt(pad, value);
        return;
    }
    if (isVuWindow(address)) {
        widenToWord(address, value, 0xFFFFu);
        return;
    }
    const u32 folded = mMirror.apply(address);
    if (mMmio != nullptr && isRegister(folded) && mMmio->writeHalf(folded, value)) {
        return;
    }
    store(resolveAddress(address, 2), value);
}

void Memory::writeVirtual32(u32 address, u32 value) {
    if (u8* pad = scratchpadAt(address, 4)) {
        storeAt(pad, value);
        return;
    }
    if (isVuWindow(address)) {
        mVuWindowWord = value;
    }
    const u32 folded = mMirror.apply(address);
    if (mMmio != nullptr && isRegister(folded) && mMmio->writeWord(folded, value)) {
        return;
    }
    store(resolveAddress(address, 4), value);
}

void Memory::writeVirtual64(u32 address, u64 value) {
    if (u8* pad = scratchpadAt(address, 8)) {
        storeAt(pad, value);
        return;
    }
    if (isVuWindow(address)) {
        mVuWindowWord = static_cast<u32>(value >> 32);
    }
    const u32 folded = mMirror.apply(address);
    if (mMmio != nullptr && isRegister(folded)) {
        if (mMmio->writeDouble(folded, value)) {
            return;
        }
        write32(address, static_cast<u32>(value));
        write32(address + 4, static_cast<u32>(value >> 32));
        return;
    }
    store(resolveAddress(address, 8), value);
}

Memory makeMainMemory() {
    std::vector<Region> regions;
    regions.push_back({0, std::vector<u8>(kMainRamSize)});
    regions.push_back({kMmioBase, std::vector<u8>(kMmioSize)});
    regions.push_back({kScratchpadBase, std::vector<u8>(kScratchpadSize)});
    regions.push_back({kVu0MicroBase, std::vector<u8>(kVu0MemorySize), kVu0Windows});
    regions.push_back({kVu0DataBase, std::vector<u8>(kVu0MemorySize), kVu0Windows});
    regions.push_back({kVu1MicroBase, std::vector<u8>(kVu1MemorySize)});
    regions.push_back({kVu1DataBase, std::vector<u8>(kVu1MemorySize)});
    regions.push_back({kGsPrivBase, std::vector<u8>(kGsPrivSize)});
    return Memory{std::move(regions), {true}};
}

Memory makeIopMemory() {
    std::vector<Region> regions;
    regions.push_back({0, std::vector<u8>(kIopRamSize)});
    regions.push_back({kIopHardwareBase, std::vector<u8>(kIopHardwareSize)});
    regions.push_back({kSpu2Base, std::vector<u8>(kSpu2Size)});
    return Memory{std::move(regions), {true}};
}

}  // namespace uee
