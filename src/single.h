#pragma once

#include "types.h"

#include <bit>

namespace uee::single {

// The PS2 single-precision format, which both coprocessors compute in. It has
// no denormals, no infinities and no NaNs, and it truncates where IEEE 754
// rounds to nearest even.

constexpr u32 kSignMask = 0x80000000u;
constexpr u32 kMagnitudeMask = 0x7FFFFFFFu;
constexpr u32 kSmallestNormal = 0x00800000u;

inline u32 flushDenormal(u32 bits) {
    const u32 magnitude = bits & kMagnitudeMask;
    const bool denormal = magnitude != 0 && magnitude < kSmallestNormal;
    return denormal ? (bits & kSignMask) : bits;
}

inline f32 toFloat(u32 bits) {
    return std::bit_cast<f32>(flushDenormal(bits));
}

inline u32 toBits(f32 value) {
    return std::bit_cast<u32>(value);
}

// A value no single float holds: high + low is the exact result.
struct Wide {
    f64 high = 0;
    f64 low = 0;
};

// Knuth's two-sum. The rounding error of a floating-point add is itself a
// floating-point number, so high + low is exactly a + b.
inline Wide addExact(f64 a, f64 b) {
    const f64 high = a + b;
    const f64 bPart = high - a;
    const f64 aPart = high - bPart;
    return {high, (a - aPart) + (b - bPart)};
}

inline u32 truncate(Wide exact) {
    const f32 nearest = static_cast<f32>(exact.high);
    const f64 back = nearest;
    const s32 side = (back > exact.high)   ? 1
                     : (back < exact.high) ? -1
                     : (exact.low < 0)     ? 1
                     : (exact.low > 0)     ? -1
                                           : 0;
    u32 bits = toBits(nearest);
    const bool overshot = (bits & kSignMask) ? (side < 0) : (side > 0);
    if (overshot && (bits & kMagnitudeMask) != 0) {
        bits -= 1;
    }
    return flushDenormal(bits);
}

inline u32 truncate(f64 exact) {
    return truncate(Wide{exact, 0});
}

}  // namespace uee::single
