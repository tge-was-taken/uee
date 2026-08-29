#include "memory.h"

#include <bit>
#include <utility>

static_assert(std::endian::native == std::endian::little,
              "guest memory is copied verbatim, so the host must be little-endian");

namespace uee {

Memory::Memory(std::vector<Region> regions, SegmentMirror mirror)
    : mRegions(std::move(regions)), mMirror(mirror) {}

Memory makeMainMemory() {
    std::vector<Region> regions;
    regions.push_back({0, std::vector<u8>(kMainRamSize)});
    regions.push_back({kMmioBase, std::vector<u8>(kMmioSize)});
    regions.push_back({kScratchpadBase, std::vector<u8>(kScratchpadSize)});
    regions.push_back({kVu0MicroBase, std::vector<u8>(kVu0MemorySize)});
    regions.push_back({kVu0DataBase, std::vector<u8>(kVu0MemorySize)});
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
