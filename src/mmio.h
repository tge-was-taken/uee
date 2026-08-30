#pragma once

#include "types.h"

#include <optional>
#include <span>

namespace uee {

constexpr u32 kMmioBase = 0x10000000;
constexpr u32 kMmioSize = 0x00010000;
constexpr u32 kGsPrivBase = 0x12000000;
constexpr u32 kGsPrivSize = 0x00002000;

struct MmioSegment {
    u32 base = 0;
    u32 size = 0;
};

// Shared by EE and IOP
class Mmio {
public:
    virtual ~Mmio() = default;

    // Read once when the device is attached, so an ordinary access only pays
    // for the range test and never for the call.
    virtual std::span<const MmioSegment> segments() const = 0;

    virtual std::optional<u32> readWord(u32 address) const = 0;
    virtual bool writeWord(u32 address, u32 value) = 0;

    // SPU2 decodes a halfword at a time.
    virtual std::optional<u16> readHalf(u32) const { return std::nullopt; }
    virtual bool writeHalf(u32, u16) { return false; }
};

}  // namespace uee
