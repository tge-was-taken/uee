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
    regions.push_back({kHardwareBase, std::vector<u8>(kHardwareSize)});
    regions.push_back({kScratchpadBase, std::vector<u8>(kScratchpadSize)});
    regions.push_back({kVu0MicroBase, std::vector<u8>(kVu0MemorySize)});
    regions.push_back({kVu0DataBase, std::vector<u8>(kVu0MemorySize)});
    regions.push_back({kGsPrivBase, std::vector<u8>(kGsPrivSize)});
    return Memory{std::move(regions), {kMirrorLimit, kPhysicalMask}};
}

}  // namespace uee
