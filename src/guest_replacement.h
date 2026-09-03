#pragma once

#include "compiler.h"
#include "host_float.h"
#include "instruction.h"
#include "memory.h"
#include "r5900.h"
#include "types.h"

#include <algorithm>
#include <bit>
#include <optional>
#include <span>
#include <string_view>

namespace uee {

// Matched by instruction hash, so a stripped program still finds it.
struct Replacement {
    u64 hash;
    std::string_view name;
    // Returns to the caller.
    std::string_view statement;
};

// Nothing when the range is not all readable.
std::optional<u64> hashInstructions(Memory& memory, u32 start, u32 end);

const Replacement* findReplacement(std::optional<u64> hash);

std::span<const Replacement> getReplacements();
}  // namespace uee

// Here so the emitted statement cannot name something that is not defined.
namespace uee::guestlib {
// One 64-bit register, not the pair a 32-bit ABI would use.
inline double getArgument(const R5900& cpu, u32 index) {
    return std::bit_cast<double>(cpu.gpr[reg::a0 + index].lo);
}

inline void setResult(R5900& cpu, double value) {
    cpu.write64(reg::v0, std::bit_cast<u64>(value));
}

inline void setResult(R5900& cpu, s32 value) {
    cpu.write32(reg::v0, static_cast<u32>(value));
}

// These round to nearest where PS2 single precision truncates.
class RoundsToNearest {
public:
    RoundsToNearest() : mSaved(host::getRounding()) { host::setRoundToNearest(); }
    ~RoundsToNearest() { host::setRounding(mSaved); }

    RoundsToNearest(const RoundsToNearest&) = delete;
    RoundsToNearest& operator=(const RoundsToNearest&) = delete;

private:
    u32 mSaved;
};

UEE_NOINLINE inline void addDouble(R5900& cpu) {
    const RoundsToNearest rounding;
    setResult(cpu, getArgument(cpu, 0) + getArgument(cpu, 1));
}

UEE_NOINLINE inline void subtractDouble(R5900& cpu) {
    const RoundsToNearest rounding;
    setResult(cpu, getArgument(cpu, 0) - getArgument(cpu, 1));
}

UEE_NOINLINE inline void multiplyDouble(R5900& cpu) {
    const RoundsToNearest rounding;
    setResult(cpu, getArgument(cpu, 0) * getArgument(cpu, 1));
}

UEE_NOINLINE inline void divideDouble(R5900& cpu) {
    const RoundsToNearest rounding;
    setResult(cpu, getArgument(cpu, 0) / getArgument(cpu, 1));
}

// Truncation, which no rounding mode changes.
UEE_NOINLINE inline void doubleToInt(R5900& cpu) {
    setResult(cpu, static_cast<s32>(getArgument(cpu, 0)));
}

UEE_NOINLINE inline void intToDouble(R5900& cpu) {
    setResult(cpu, static_cast<double>(static_cast<s32>(low(cpu.gpr[reg::a0]))));
}

// Widening is exact whatever the mode; narrowing is not.
UEE_NOINLINE inline void floatToDouble(R5900& cpu) {
    setResult(cpu, static_cast<double>(std::bit_cast<float>(low(cpu.gpr[reg::a0]))));
}

UEE_NOINLINE inline void doubleToFloat(R5900& cpu) {
    const RoundsToNearest rounding;
    cpu.write32(reg::v0, std::bit_cast<u32>(static_cast<float>(getArgument(cpu, 0))));
}

// Answers 1 where the two do not order, which fails every comparison.
UEE_NOINLINE inline void compareDouble(R5900& cpu) {
    const double left = getArgument(cpu, 0);
    const double right = getArgument(cpu, 1);
    if (left < right) {
        setResult(cpu, -1);
    } else if (left == right) {
        setResult(cpu, 0);
    } else {
        setResult(cpu, 1);
    }
}

// subu on two lbu, so the bytes are unsigned and this is their difference.
inline void setDifferenceResult(R5900& cpu, u8 left, u8 right) {
    cpu.write32(reg::v0, static_cast<u32>(static_cast<s32>(left) - static_cast<s32>(right)));
}

UEE_NOINLINE inline void strcmp(R5900& cpu, Memory& memory) {
    u32 left = static_cast<u32>(cpu.gpr[reg::a0].lo);
    u32 right = static_cast<u32>(cpu.gpr[reg::a1].lo);
    const std::span<u8> ram = memory.regionAt(0);
    if (left < ram.size() && right < ram.size()) {
        const u8* a = ram.data() + left;
        const u8* b = ram.data() + right;
        const usize reach = ram.size() - std::max(left, right);
        for (usize i = 0; i < reach; ++i) {
            if (a[i] != b[i] || a[i] == 0) {
                setDifferenceResult(cpu, a[i], b[i]);
                return;
            }
        }
    }
    // Somewhere other than main memory, or a string running to the end of it.
    for (;;) {
        const u8 a = memory.read8(left++);
        const u8 b = memory.read8(right++);
        if (a != b || a == 0) {
            setDifferenceResult(cpu, a, b);
            return;
        }
    }
}
}  // namespace uee::guestlib
