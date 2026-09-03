#pragma once

#include "types.h"

#include <bit>
#include <cmath>

namespace uee::single {

// Every pattern is an ordinary number: exponent 255 is a real exponent, not an
// infinity or NaN, denormals read as zero, and results saturate. Kept as bit
// patterns because a host float cannot hold the top exponent.

constexpr u32 kSignMask = 0x80000000u;
constexpr u32 kMagnitudeMask = 0x7FFFFFFFu;
constexpr u32 kSmallestNormal = 0x00800000u;
constexpr u32 kLargest = 0x7FFFFFFFu;
// The VU saturates one exponent lower than the FPU: it treats exponent 255 as
// out of range where the FPU treats it as an ordinary exponent.
constexpr u32 kVuLargest = 0x7F7FFFFFu;

inline u32 flushDenormal(u32 bits) {
    const u32 magnitude = bits & kMagnitudeMask;
    const bool denormal = magnitude != 0 && magnitude < kSmallestNormal;
    return denormal ? (bits & kSignMask) : bits;
}

// ldexp and frexp do not inline, and this is the path every result takes.
inline f64 twoTo(s32 exponent) {
    return std::bit_cast<f64>(static_cast<u64>(exponent + 1023) << 52);
}

// Everything rounded here is small enough for the conversion alone.
inline f64 roundDown(f64 value) {
    const f64 truncated = static_cast<f64>(static_cast<s64>(value));
    return truncated > value ? truncated - 1.0 : truncated;
}

// What frexp reports: the magnitude is its mantissa in [0.5, 1) times this.
inline s32 binaryExponent(f64 magnitude) {
    return static_cast<s32>((std::bit_cast<u64>(magnitude) >> 52) & 0x7FFu) - 1022;
}

inline f64 widen(u32 bits) {
    const u32 exponent = (bits >> 23) & 0xFFu;
    const f64 sign = (bits & kSignMask) ? -1.0 : 1.0;
    if (exponent == 0) {
        return sign * 0.0;
    }
    return sign * static_cast<f64>(0x800000u | (bits & 0x7FFFFFu)) *
           twoTo(static_cast<s32>(exponent) - 150);
}

// Nothing here is unordered, so once denormals flush the patterns are a total
// order and a comparison is an integer one.
inline s32 orderKey(u32 bits) {
    const u32 clean = flushDenormal(bits);
    return (clean & kSignMask) ? -static_cast<s32>(clean & kMagnitudeMask)
                               : static_cast<s32>(clean);
}

// max.s and min.s do see the sign of a zero, so they need an order that keeps
// the two zeros apart.
inline s32 signedOrderKey(u32 bits) {
    const u32 clean = flushDenormal(bits);
    return (clean & kSignMask) ? ~static_cast<s32>(clean & kMagnitudeMask)
                               : static_cast<s32>(clean);
}

// A value no single holds: high + low is the exact result.
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

enum class Rounding {
    Truncate,
    Nearest,
};

// The stored pattern plus whether it left the exponent range, which is what
// the FPU raises its overflow and underflow flags from.
struct Rounded {
    u32 bits = 0;
    bool overflow = false;
    bool underflow = false;
};

inline Rounded join(u32 sign, s32 exponent, u32 significand) {
    if (significand == 0) {
        return {sign, false, false};
    }
    if (exponent > 255) {
        return {sign | kLargest, true, false};
    }
    if (exponent < 1) {
        return {sign, false, true};
    }
    return {sign | (static_cast<u32>(exponent) << 23) | (significand & 0x7FFFFFu), false, false};
}

// The rounding is fixed at every call, so each one gets the arithmetic it
// needs rather than a branch between two.
template <Rounding rounding>
inline Rounded pack(Wide exact) {
    const bool negative = std::signbit(exact.high);
    const u32 sign = negative ? kSignMask : 0u;
    const f64 magnitude = std::fabs(exact.high);
    if (magnitude == 0.0) {
        return {sign, false, false};
    }

    s32 exponent = binaryExponent(magnitude);
    const f64 scale = twoTo(24 - exponent);
    const f64 scaled = magnitude * scale;
    f64 significand = roundDown(scaled);
    const f64 residual = negative ? -exact.low : exact.low;
    const f64 leftover = (scaled - significand) + residual * scale;

    if constexpr (rounding == Rounding::Nearest) {
        significand += roundDown(leftover + 0.5);
    } else {
        significand += roundDown(std::min(leftover, 0.0));
    }
    if (significand >= 16777216.0) {
        significand = 8388608.0;
        exponent += 1;
    } else if (significand < 8388608.0) {
        significand = 16777215.0;
        exponent -= 1;
    }

    return join(sign, exponent + 126, static_cast<u32>(significand));
}

inline Rounded truncate(Wide exact) {
    return pack<Rounding::Truncate>(exact);
}

inline Rounded truncate(f64 exact) {
    return pack<Rounding::Truncate>(Wide{exact, 0});
}

struct Parts {
    u32 sign;
    s32 exponent;
    u32 significand;  // 24 bits with the implicit one, zero for a zero
};

inline Parts split(u32 bits) {
    const u32 clean = flushDenormal(bits);
    const u32 exponent = (clean >> 23) & 0xFFu;
    return {clean & kSignMask, static_cast<s32>(exponent),
            exponent == 0 ? 0u : (0x800000u | (clean & 0x7FFFFFu))};
}

// The adder holds no bits below the significand, so alignment discards them.
inline Rounded addParts(Parts a, Parts b) {
    if (a.significand == 0 && b.significand == 0) {
        return {a.sign & b.sign, false, false};
    }
    if (a.significand == 0) {
        return join(b.sign, b.exponent, b.significand);
    }
    if (b.significand == 0) {
        return join(a.sign, a.exponent, a.significand);
    }

    if (a.exponent < b.exponent) {
        const Parts swapped = a;
        a = b;
        b = swapped;
    }

    const s32 shift = a.exponent - b.exponent;
    const u32 aligned = shift >= 24 ? 0u : (b.significand >> shift);

    s32 exponent = a.exponent;
    u32 sign = a.sign;
    u32 sum = 0;

    if (a.sign == b.sign) {
        sum = a.significand + aligned;
        if (sum & 0x1000000u) {
            sum >>= 1;
            ++exponent;
        }
    } else if (a.significand >= aligned) {
        sum = a.significand - aligned;
    } else {
        sum = aligned - a.significand;
        sign = b.sign;
    }

    if (sum == 0) {
        return {};
    }
    // Renormalising: the sum is at most 24 bits, so where its top bit sits says
    // how far it has to travel in one go.
    const s32 normalise = std::countl_zero(sum) - 8;
    return join(sign, exponent - normalise, sum << normalise);
}

// The unit truncates 48 bits back to 24. The exponent is left unbounded so a
// caller can accumulate before the range check, which is what madd.s does.
//
// Not commutative: an all-ones second operand costs the product its low bit
// when nothing carried.
inline Parts mulParts(Parts a, Parts b) {
    if (a.significand == 0 || b.significand == 0) {
        return {a.sign ^ b.sign, 0, 0};
    }
    const u64 wide = static_cast<u64>(a.significand) * b.significand;
    const bool carried = (wide >> 47) != 0;
    u32 significand = static_cast<u32>(wide >> (carried ? 24 : 23));
    if (!carried && significand == 0xFFFFFFu && b.significand == 0xFFFFFFu) {
        significand = 0xFFFFFEu;
    }
    return {a.sign ^ b.sign, a.exponent + b.exponent - 127 + (carried ? 1 : 0), significand};
}

inline Rounded add(u32 a, u32 b) {
    return addParts(split(a), split(b));
}

inline Rounded sub(u32 a, u32 b) {
    Parts negated = split(b);
    negated.sign ^= kSignMask;
    return addParts(split(a), negated);
}

inline Rounded mul(u32 a, u32 b) {
    const Parts product = mulParts(split(a), split(b));
    return join(product.sign, product.exponent, product.significand);
}

// The accumulate keeps the product at its true exponent, so a product that
// would have saturated on its own still dominates the sum.
inline Rounded multiplyAdd(u32 acc, u32 a, u32 b, bool subtract) {
    Parts product = mulParts(split(a), split(b));
    if (subtract) {
        product.sign ^= kSignMask;
    }
    return addParts(split(acc), product);
}

// The divider rounds to nearest where the adder truncates, and answers a zero
// denominator with the largest pattern. Divide-by-zero has its own flag.
inline Rounded div(u32 a, u32 b) {
    const f64 denominator = widen(b);
    if (denominator == 0.0) {
        return {((a ^ b) & kSignMask) | kLargest, false, false};
    }
    const f64 numerator = widen(a);
    const f64 quotient = numerator / denominator;
    const f64 residual = std::fma(-quotient, denominator, numerator);
    return {pack<Rounding::Nearest>(Wide{quotient, residual / denominator}).bits, false, false};
}

// The unit takes the root of the magnitude; the format has no NaN to return.
inline Rounded sqrt(u32 a) {
    const f64 radicand = std::fabs(widen(a));
    if (radicand == 0.0) {
        return {};
    }
    const f64 root = std::sqrt(radicand);
    const f64 residual = std::fma(-root, root, radicand);
    return pack<Rounding::Nearest>(Wide{root, residual / (2.0 * root)});
}

// The sign comes from the numerator alone, because the root took a magnitude.
inline Rounded rsqrt(u32 a, u32 b) {
    const u32 sign = a & kSignMask;
    const f64 root = widen(sqrt(b).bits);
    if (root == 0.0) {
        return {sign | kLargest, false, false};
    }
    const f64 reciprocal = 1.0 / root;
    const f64 reciprocalResidual = std::fma(-reciprocal, root, 1.0);
    const u32 scale = pack<Rounding::Nearest>(Wide{reciprocal, reciprocalResidual / root}).bits;
    const f64 factor = widen(scale);
    const f64 numerator = std::fabs(widen(a));
    const f64 product = numerator * factor;
    const f64 residual = std::fma(numerator, factor, -product);
    Rounded result = pack<Rounding::Truncate>(Wide{product, residual});
    result.bits |= sign;
    return result;
}

// Comparing patterns keeps the top exponent away from the host FPU, where it
// would read as an infinity or a NaN.
inline s32 toInt32(u32 bits) {
    const u32 clean = flushDenormal(bits);
    if ((clean & kMagnitudeMask) >= 0x4F000000u) {
        return (clean & kSignMask) ? INT32_MIN : INT32_MAX;
    }
    return static_cast<s32>(std::bit_cast<f32>(clean));
}

inline u32 clampToVu(u32 bits) {
    return ((bits & kMagnitudeMask) > kVuLargest) ? ((bits & kSignMask) | kVuLargest) : bits;
}

inline u32 fromInt32(s32 value) {
    return truncate(static_cast<f64>(value)).bits;
}

}  // namespace uee::single
