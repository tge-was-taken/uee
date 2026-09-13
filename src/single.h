#pragma once

#include "compiler.h"
#include "types.h"

#include <algorithm>
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
constexpr u32 kVuLargest = 0x7FFFFFFFu;

inline u32 flushDenormal(u32 bits) {
    const u32 magnitude = bits & kMagnitudeMask;
    const bool denormal = magnitude != 0 && magnitude < kSmallestNormal;
    return denormal ? (bits & kSignMask) : bits;
}

// ldexp and frexp do not inline, and this is the path every result takes.
inline f64 getPowerOfTwo(s32 exponent) {
    return std::bit_cast<f64>(static_cast<u64>(exponent + 1023) << 52);
}

// Everything rounded here is small enough for the conversion alone.
inline f64 roundDown(f64 value) {
    const f64 truncated = static_cast<f64>(static_cast<s64>(value));
    return truncated > value ? truncated - 1.0 : truncated;
}

// What frexp reports: the magnitude is its mantissa in [0.5, 1) times this.
inline s32 getBinaryExponent(f64 magnitude) {
    return static_cast<s32>((std::bit_cast<u64>(magnitude) >> 52) & 0x7FFu) - 1022;
}

inline f64 widen(u32 bits) {
    const u32 exponent = (bits >> 23) & 0xFFu;
    const f64 sign = (bits & kSignMask) ? -1.0 : 1.0;
    if (exponent == 0) {
        return sign * 0.0;
    }
    return sign * static_cast<f64>(0x800000u | (bits & 0x7FFFFFu)) *
           getPowerOfTwo(static_cast<s32>(exponent) - 150);
}

// Nothing here is unordered, so once denormals flush the patterns are a total
// order and a comparison is an integer one.
inline s32 getOrderKey(u32 bits) {
    const u32 clean = flushDenormal(bits);
    return (clean & kSignMask) ? -static_cast<s32>(clean & kMagnitudeMask)
                               : static_cast<s32>(clean);
}

// The vector unit's max and mini order the patterns as they stand, keeping the
// two zeros apart and denormals intact.
inline s32 getVuOrderKey(u32 bits) {
    return (bits & kSignMask) ? ~static_cast<s32>(bits & kMagnitudeMask)
                              : static_cast<s32>(bits);
}

// max.s and min.s do see the sign of a zero, so they need an order that keeps
// the two zeros apart.
inline s32 getSignedOrderKey(u32 bits) {
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

inline Rounded join(Parts value) {
    return join(value.sign, value.exponent, value.significand);
}

inline f64 widenParts(Parts value) {
    if (value.significand == 0) {
        return (value.sign ? -1.0 : 1.0) * 0.0;
    }
    return (value.sign ? -1.0 : 1.0) * static_cast<f64>(value.significand) *
           getPowerOfTwo(value.exponent - 150);
}

// The rounding is fixed at every call, so each one gets the arithmetic it
// needs rather than a branch between two.
template <Rounding rounding>
inline Parts packParts(Wide exact) {
    const bool negative = std::signbit(exact.high);
    const u32 sign = negative ? kSignMask : 0u;
    const f64 magnitude = std::fabs(exact.high);
    if (magnitude == 0.0) {
        return {sign, 0, 0};
    }

    s32 exponent = getBinaryExponent(magnitude);
    const f64 scale = getPowerOfTwo(24 - exponent);
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

    return {sign, exponent + 126, static_cast<u32>(significand)};
}

template <Rounding rounding>
inline Rounded pack(Wide exact) {
    return join(packParts<rounding>(exact));
}

inline Rounded truncate(Wide exact) {
    return pack<Rounding::Truncate>(exact);
}

inline Rounded truncate(f64 exact) {
    return pack<Rounding::Truncate>(Wide{exact, 0});
}

// The adder holds no bits below the significand, so alignment discards them.
inline Parts addToParts(Parts a, Parts b) {
    if (a.significand == 0 && b.significand == 0) {
        return {a.sign & b.sign, 0, 0};
    }
    if (a.significand == 0) {
        return b;
    }
    if (b.significand == 0) {
        return a;
    }

    if (a.exponent < b.exponent) {
        const Parts swapped = a;
        a = b;
        b = swapped;
    }

    // One bit below the significand is kept through the alignment, which is what
    // lets a subtraction borrow from an operand that would otherwise shift away
    // entirely.
    const s32 shift = a.exponent - b.exponent;
    const u32 wide = a.significand << 1;
    const u32 aligned = shift >= 25 ? 0u : ((b.significand << 1) >> shift);

    s32 exponent = a.exponent;
    u32 sign = a.sign;
    u32 sum = 0;

    if (a.sign == b.sign) {
        sum = wide + aligned;
        if (sum & 0x2000000u) {
            sum >>= 1;
            ++exponent;
        }
    } else if (wide >= aligned) {
        sum = wide - aligned;
    } else {
        sum = aligned - wide;
        sign = b.sign;
    }

    if (sum == 0) {
        return {0, 0, 0};
    }
    // Renormalising: where the top bit sits says how far the sum has to travel,
    // and the guard bit is dropped rather than rounded.
    const s32 normalise = std::countl_zero(sum) - 7;
    return {sign, exponent - normalise, (sum << normalise) >> 1};
}

inline Rounded addParts(Parts a, Parts b) {
    return join(addToParts(a, b));
}

// The booth tree skips a few low adds, so bit 15 can borrow upward. The recode
// reads the second operand, which makes the multiply not commutative.
// From the reverse engineered EE multiplier in PCSX2 PR #12001, PS2Float.cpp.
struct BoothDigit {
    u32 data;
    u32 negate;
};

inline BoothDigit computeBoothDigit(u32 a, u32 b, u32 place) {
    const u32 test = (place != 0 ? b >> (place * 2 - 1) : b << 1) & 7u;
    u32 value = a << (place * 2);
    value += (test == 3 || test == 4) ? value : 0u;
    const u32 negate = (test >= 4 && test <= 6) ? ~0u : 0u;
    const u32 place1 = 1u << (place * 2);
    value ^= negate & -place1;
    value &= (test >= 1 && test <= 6) ? ~0u : 0u;
    return {value, negate & place1};
}

struct CarrySave {
    u32 sum;
    u32 carry;
};

inline CarrySave addThree(u32 a, u32 b, u32 c) {
    const u32 u = a ^ b;
    return {u ^ c, ((u & c) | (a & b)) << 1};
}

UEE_NOINLINE inline u64 computeBoothProduct(u32 a, u32 b, u64 exact) {
    const BoothDigit d0 = computeBoothDigit(a, b, 0);
    const BoothDigit d1 = computeBoothDigit(a, b, 1);
    const BoothDigit d2 = computeBoothDigit(a, b, 2);
    const BoothDigit d3 = computeBoothDigit(a, b, 3);
    const BoothDigit d4 = computeBoothDigit(a, b, 4);
    const BoothDigit d5 = computeBoothDigit(a, b, 5);
    const BoothDigit d6 = computeBoothDigit(a, b, 6);
    BoothDigit d7 = computeBoothDigit(a, b, 7);

    const CarrySave t0 = addThree(d1.data, d2.data, d3.data);
    CarrySave t1 = addThree(d4.data & ~0x7FFu, d5.data & ~0xFFFu, d6.data);
    t1.carry |= d6.negate | (d5.data & 0x800u);
    d7.data |= (d5.data & 0x400u) + d5.negate;

    const CarrySave t2 = addThree(d0.data, t0.sum, t0.carry);
    const CarrySave t3 = addThree(d7.data, t1.sum, t1.carry);
    const CarrySave t4 = addThree(t2.carry, t3.sum, t3.carry);
    CarrySave t5 = addThree(t2.sum, t4.sum, t4.carry);

    t5.carry += d7.negate;
    const u32 low = (t5.sum & ~0x7FFFu) + (t5.carry & ~0x7FFFu);
    return exact - ((low ^ static_cast<u32>(exact)) & 0x8000u);
}

// A borrow out of bit 15 only reaches the result when everything between is
// zero, and a second operand with no low bits recodes to no digits at all.
inline bool canBorrowLowBit(u32 b, u64 exact) {
    return (exact & 0x7FFFFFu) < 0x8000u && (b & 0xFFFFu) != 0;
}

// The unit truncates 48 bits back to 24. The exponent is left unbounded so a
// caller can accumulate before the range check, which is what madd.s does.
inline Parts multiplyParts(Parts a, Parts b) {
    if (a.significand == 0 || b.significand == 0) {
        return {a.sign ^ b.sign, 0, 0};
    }
    const u64 exact = static_cast<u64>(a.significand) * b.significand;
    const u64 wide = canBorrowLowBit(b.significand, exact)
                         ? computeBoothProduct(a.significand, b.significand, exact)
                         : exact;
    const bool carried = (wide >> 47) != 0;
    const u32 significand = static_cast<u32>(wide >> (carried ? 24 : 23));
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
    const Parts product = multiplyParts(split(a), split(b));
    return join(product);
}

// The accumulate keeps the product at its true exponent, so a product that
// would have saturated on its own still dominates the sum.
inline Rounded multiplyAdd(u32 acc, u32 a, u32 b, bool subtract) {
    Parts product = multiplyParts(split(a), split(b));
    if (subtract) {
        product.sign ^= kSignMask;
    }
    return addParts(split(acc), product);
}

// Radix-2 non-restoring, carry-save, 25 digits and no rounding step: the answer
// is the exact floor or one above it, depending on the residual.
// From the reverse engineered EE divider in PCSX2 PR #12001, PS2Float.cpp.
inline s32 selectDigit(CarrySave current) {
    constexpr u32 mask = (1u << 24) - 1;
    const s32 test = static_cast<s32>(((current.sum & ~mask) + current.carry) |
                                      (current.sum & mask));
    if (test >= 1 << 23) {
        return 1;
    }
    if (test < static_cast<s32>(~0u << 24)) {
        return -1;
    }
    return 0;
}

inline u32 getStoredSignificand(u32 bits) {
    return (bits & 0x7FFFFFu) | 0x800000u;
}

inline Rounded div(u32 a, u32 b) {
    const u32 sign = (a ^ b) & kSignMask;
    const u32 numeratorExponent = (flushDenormal(a) >> 23) & 0xFFu;
    const u32 denominatorExponent = (flushDenormal(b) >> 23) & 0xFFu;
    if (denominatorExponent == 0) {
        return {sign | kLargest, false, false};
    }
    if (numeratorExponent == 0) {
        return {sign, false, false};
    }

    const u32 divisor = getStoredSignificand(a) << 2;
    const u32 dividend = getStoredSignificand(b) << 2;
    CarrySave current{divisor, 0};
    u32 quotient = 0;
    s32 digit = 1;
    for (s32 step = 0; step < 25; ++step) {
        quotient = (quotient << 1) + static_cast<u32>(digit);
        const u32 addend = digit > 0 ? ~dividend : (digit < 0 ? dividend : 0u);
        current.carry += digit > 0 ? 1u : 0u;
        const CarrySave next = addThree(current.sum, current.carry, addend);
        digit = selectDigit(digit != 0 ? next : current);
        current.sum = next.sum << 1;
        current.carry = next.carry << 1;
    }

    s32 exponent = static_cast<s32>(numeratorExponent) -
                   static_cast<s32>(denominatorExponent) + 126;
    if (quotient >= (1u << 24)) {
        exponent += 1;
        quotient >>= 1;
    }
    // The divider reports neither end of the range: only the two conditions its
    // own flags name.
    return {join(sign, exponent, quotient).bits, false, false};
}

// The unit takes the root of the magnitude; the format has no NaN to return.
inline Rounded sqrt(u32 a) {
    const u32 clean = flushDenormal(a);
    if ((clean & 0x7F800000u) == 0) {
        return {};
    }
    u32 radicand = getStoredSignificand(clean) << 1;
    if ((clean & 0x800000u) == 0) {
        radicand <<= 1;
    }
    CarrySave current{radicand, 0};
    u32 root = 0;
    s32 digit = 1;
    for (s32 step = 0; step < 25; ++step) {
        const u32 trial = root + (static_cast<u32>(digit) << (24 - step));
        root += static_cast<u32>(digit) << (25 - step);
        const u32 addend = digit > 0 ? ~trial : (digit < 0 ? trial : 0u);
        current.carry += digit > 0 ? 1u : 0u;
        const CarrySave next = addThree(current.sum, current.carry, addend);
        digit = selectDigit(digit != 0 ? next : current);
        current.sum = next.sum << 1;
        current.carry = next.carry << 1;
    }
    const u32 exponent = (((clean >> 23) & 0xFFu) + 127) >> 1;
    return {((root >> 2) & 0x7FFFFFu) | (exponent << 23), false, false};
}

// The sign comes from the numerator alone, because the root took a magnitude.
inline Rounded rsqrt(u32 a, u32 b) {
    const u32 root = sqrt(b).bits;
    Rounded result = div(a & kMagnitudeMask, root);
    result.bits |= a & kSignMask;
    return result;
}

// Comparing patterns keeps the top exponent away from the host FPU, where it
// would read as an infinity or a NaN.
inline s32 convertToInt32(u32 bits) {
    const u32 clean = flushDenormal(bits);
    if ((clean & kMagnitudeMask) >= 0x4F000000u) {
        return (clean & kSignMask) ? INT32_MIN : INT32_MAX;
    }
    return static_cast<s32>(std::bit_cast<f32>(clean));
}

inline u32 clampToVu(u32 bits) {
    return ((bits & kMagnitudeMask) > kVuLargest) ? ((bits & kSignMask) | kVuLargest) : bits;
}

inline u32 convertFromInt32(s32 value) {
    return truncate(static_cast<f64>(value)).bits;
}

}  // namespace uee::single
