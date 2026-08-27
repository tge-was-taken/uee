#include "elf.h"

#include <format>
#include <stdexcept>

namespace uee {
namespace {

constexpr usize kEntryOffset = 24;
constexpr usize kProgramHeaderOffset = 28;
constexpr usize kProgramHeaderSizeOffset = 42;
constexpr usize kProgramHeaderCountOffset = 44;

constexpr u32 kLoadableSegment = 1;
constexpr u16 kExecutable = 2;
constexpr u16 kMachineMips = 8;

u32 readU32(std::span<const u8> image, usize offset) {
    if (offset + 4 > image.size()) {
        throw std::runtime_error(std::format("ELF ends before offset {}", offset));
    }
    return static_cast<u32>(image[offset]) | (static_cast<u32>(image[offset + 1]) << 8) |
           (static_cast<u32>(image[offset + 2]) << 16) |
           (static_cast<u32>(image[offset + 3]) << 24);
}

u16 readU16(std::span<const u8> image, usize offset) {
    if (offset + 2 > image.size()) {
        throw std::runtime_error(std::format("ELF ends before offset {}", offset));
    }
    return static_cast<u16>(image[offset] | (image[offset + 1] << 8));
}

void requireHeader(std::span<const u8> image) {
    if (image.size() < 52) {
        throw std::runtime_error(std::format("ELF is {} bytes, too short for a header",
                                             image.size()));
    }
    if (image[0] != 0x7F || image[1] != 'E' || image[2] != 'L' || image[3] != 'F') {
        throw std::runtime_error("ELF magic missing");
    }
    if (image[4] != 1) {
        throw std::runtime_error(std::format("ELF class {} is not 32-bit", image[4]));
    }
    if (image[5] != 1) {
        throw std::runtime_error(std::format("ELF data encoding {} is not little-endian",
                                             image[5]));
    }
    const u16 type = readU16(image, 16);
    if (type != kExecutable) {
        throw std::runtime_error(std::format("ELF type {} is not an executable", type));
    }
    const u16 machine = readU16(image, 18);
    if (machine != kMachineMips) {
        throw std::runtime_error(std::format("ELF machine {} is not MIPS", machine));
    }
}

void copySegment(std::span<const u8> image, Memory& memory, usize headerOffset) {
    const u32 fileOffset = readU32(image, headerOffset + 4);
    const u32 address = readU32(image, headerOffset + 8);
    const u32 fileSize = readU32(image, headerOffset + 16);
    const u32 memorySize = readU32(image, headerOffset + 20);

    if (fileOffset + fileSize > image.size()) {
        throw std::runtime_error(std::format(
            "segment at {:08x} reads {} bytes from offset {}, past the end of a {} byte ELF",
            address, fileSize, fileOffset, image.size()));
    }
    if (memorySize < fileSize) {
        throw std::runtime_error(std::format(
            "segment at {:08x} occupies {} bytes but carries {}", address, memorySize, fileSize));
    }

    for (u32 i = 0; i < fileSize; ++i) {
        memory.write8(address + i, image[fileOffset + i]);
    }
    for (u32 i = fileSize; i < memorySize; ++i) {
        memory.write8(address + i, 0);
    }
}

}  // namespace

u32 loadElf(std::span<const u8> image, Memory& memory) {
    requireHeader(image);

    const u32 tableOffset = readU32(image, kProgramHeaderOffset);
    const u16 entrySize = readU16(image, kProgramHeaderSizeOffset);
    const u16 entryCount = readU16(image, kProgramHeaderCountOffset);

    for (u16 i = 0; i < entryCount; ++i) {
        const usize headerOffset = tableOffset + static_cast<usize>(i) * entrySize;
        if (readU32(image, headerOffset) == kLoadableSegment) {
            copySegment(image, memory, headerOffset);
        }
    }

    return readU32(image, kEntryOffset);
}

}  // namespace uee
