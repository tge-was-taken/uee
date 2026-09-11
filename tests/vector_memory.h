#pragma once

#include "memory.h"
#include "types.h"

#include <format>
#include <string>

namespace uee::test {

// The recorders write a memory quadword as its sixteen bytes in address order.
inline u32 readLittleEndianWord(const std::string& bytes, u32 word) {
    u32 value = 0;
    for (u32 i = 0; i < 4; ++i) {
        const u32 at = (word * 4 + i) * 2;
        value |= static_cast<u32>(std::stoul(bytes.substr(at, 2), nullptr, 16)) << (8 * i);
    }
    return value;
}

// A narrower write to a VU window fills the rest of its word with ones, so a
// quadword goes in and comes out a word at a time.
inline void writeQuadword(Memory& memory, u32 address, const std::string& bytes) {
    for (u32 i = 0; i < 4; ++i) {
        memory.write32(address + i * 4, readLittleEndianWord(bytes, i));
    }
}

inline std::string readQuadword(Memory& memory, u32 address) {
    std::string bytes;
    for (u32 i = 0; i < 4; ++i) {
        const u32 word = memory.read32(address + i * 4);
        for (u32 b = 0; b < 4; ++b) {
            bytes += std::format("{:02X}", (word >> (8 * b)) & 0xFFu);
        }
    }
    return bytes;
}

}  // namespace uee::test
