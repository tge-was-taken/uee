#pragma once

#include "arch.h"
#include "types.h"

#include <bit>
#include <optional>

// Wrong unless configureHostFloat has run on this thread.
namespace uee::host {

constexpr bool kHasFloatUnit = arch::kHasFloatUnit;

using arch::getRounding;
using arch::setRounding;
using arch::setRoundToNearest;
using arch::Lanes;

#ifdef UEE_HOST_FPU

constexpr u32 kSignBit = 0x80000000u;

// Rounding toward zero, denormals flushed on the way in and out. The mode is
// per thread, so every thread that runs guest code calls this.
inline void configureHostFloat() {
    constexpr u32 kFlushToZero = 0x8000u;
    constexpr u32 kDenormalsAreZero = 0x0040u;
    constexpr u32 kRoundToZero = 3u << 13;
    arch::setFloatControl(arch::getFloatControl() | kFlushToZero | kDenormalsAreZero |
                          kRoundToZero);
}

// One below the top exponent, so a carry out of the add cannot reach it.
constexpr u32 kHighestOperand = 253;

constexpr u32 exponentOf(u32 bits) { return (bits >> 23) & 0xFFu; }

// The unit drops whatever the alignment shift would have shifted out.
inline void alignOperands(u32& a, u32& b) {
    const s32 shift = static_cast<s32>(exponentOf(a)) - static_cast<s32>(exponentOf(b));
    const u32 places = static_cast<u32>(shift < 0 ? -shift : shift);
    const u32 drop = ~0u << (places > 23 ? 31 : places);
    a &= shift < 0 ? drop : ~0u;
    b &= shift > 0 ? drop : ~0u;
}

// A zero or denormal result is one the unit flags, so it is not answered here.
inline bool isPlainResult(u32 bits) {
    const u32 exponent = exponentOf(bits);
    return exponent != 0 && exponent <= kHighestOperand;
}

inline std::optional<u32> add(u32 a, u32 b) {
    if (exponentOf(a) > kHighestOperand || exponentOf(b) > kHighestOperand) {
        return std::nullopt;
    }
    alignOperands(a, b);
    const u32 sum = std::bit_cast<u32>(std::bit_cast<float>(a) + std::bit_cast<float>(b));
    return isPlainResult(sum) ? std::optional(sum) : std::nullopt;
}

inline std::optional<u32> sub(u32 a, u32 b) {
    return add(a, b ^ kSignBit);
}

inline std::optional<u32> mul(u32 a, u32 b) {
    if (exponentOf(a) > kHighestOperand || exponentOf(b) > kHighestOperand ||
        losesLowBit(a, b)) {
        return std::nullopt;
    }
    const u32 product = std::bit_cast<u32>(std::bit_cast<float>(a) * std::bit_cast<float>(b));
    return isPlainResult(product) ? std::optional(product) : std::nullopt;
}

// Only offered where the result comes out plain.
inline std::optional<u32> multiplyAdd(u32 acc, u32 a, u32 b, bool subtract) {
    if (exponentOf(acc) > kHighestOperand || exponentOf(a) > kHighestOperand ||
        exponentOf(b) > kHighestOperand || losesLowBit(a, b)) {
        return std::nullopt;
    }
    const u32 product = std::bit_cast<u32>(std::bit_cast<float>(a) * std::bit_cast<float>(b));
    if (!isPlainResult(product)) {
        return std::nullopt;
    }
    u32 left = acc;
    u32 right = subtract ? product ^ kSignBit : product;
    alignOperands(left, right);
    const u32 sum =
        std::bit_cast<u32>(std::bit_cast<float>(left) + std::bit_cast<float>(right));
    return isPlainResult(sum) ? std::optional(sum) : std::nullopt;
}

inline Lanes ceilingLanes() {
    return Lanes::broadcast(kHighestOperand);
}

// The vector unit keeps no overflow flag, so only a saturating result is out.
inline bool areLanesPlain(Lanes result) {
    return !arch::anyLaneSet(arch::greaterThanSigned(result.exponents(), ceilingLanes()));
}

// The multiplier's tree only moves the product when a borrow out of bit 15 can
// reach the result, and that needs every bit between to be zero.
inline bool losesLowBit(u32 a, u32 b) {
    const u64 exact = static_cast<u64>(0x800000u | (a & 0x7FFFFFu)) *
                      (0x800000u | (b & 0x7FFFFFu));
    return (exact & 0x7FFFFFu) < 0x8000u;
}

inline bool anyLoseLowBit(Lanes a, Lanes b) {
    const Lanes significand = Lanes::broadcast(0x7FFFFF);
    const Lanes implicit = Lanes::broadcast(0x800000);
    const Lanes sa = arch::either(arch::maskWith(a, significand), implicit);
    const Lanes sb = arch::either(arch::maskWith(b, significand), implicit);
    const Lanes low = arch::maskWith(arch::multiplyLowEach(sa, sb), significand);
    return arch::anyLaneSet(arch::greaterThanSigned(Lanes::broadcast(0x8000), low));
}

inline bool anyOperandTooLarge(Lanes a, Lanes b) {
    return arch::anyLaneSet(arch::either(
        arch::greaterThanSigned(a.exponents(), ceilingLanes()),
        arch::greaterThanSigned(b.exponents(), ceilingLanes())));
}

inline bool addLanes(Lanes a, Lanes b, u32 out[4]) {
    if (anyOperandTooLarge(a, b)) {
        return false;
    }

    const Lanes shift = arch::subtractEach(a.exponents(), b.exponents());
    const Lanes places = arch::absoluteEach(shift);
    const Lanes clamped = arch::choose(places, Lanes::broadcast(31),
                                       arch::greaterThanSigned(places, Lanes::broadcast(23)));
    const Lanes drop = arch::shiftLeftEach(Lanes::broadcast(~0u), clamped);
    const Lanes keep = Lanes::broadcast(~0u);
    const Lanes zero = Lanes::broadcast(0);
    const Lanes alignedA =
        arch::maskWith(a, arch::choose(keep, drop, arch::greaterThanSigned(zero, shift)));
    const Lanes alignedB =
        arch::maskWith(b, arch::choose(keep, drop, arch::greaterThanSigned(shift, zero)));

    const Lanes sum = arch::addFloat(alignedA, alignedB);
    if (!areLanesPlain(sum)) {
        return false;
    }
    sum.store(out);
    return true;
}

inline bool subLanes(Lanes a, Lanes b, u32 out[4]) {
    return addLanes(a, arch::flipBits(b, Lanes::broadcast(kSignBit)), out);
}

inline bool mulLanes(Lanes a, Lanes b, u32 out[4]) {
    if (anyOperandTooLarge(a, b) || anyLoseLowBit(a, b)) {
        return false;
    }
    const Lanes product = arch::multiplyFloat(a, b);
    if (!areLanesPlain(product)) {
        return false;
    }
    product.store(out);
    return true;
}

// One rounding is what the unit does, which is what an FMA gives, apart from
// the low bit the multiplier's tree can drop.
inline bool maddLanes(Lanes acc, Lanes a, Lanes b, u32 out[4], bool subtract) {
    const Lanes highest =
        arch::maxUnsigned(arch::maxUnsigned(a.exponents(), b.exponents()), acc.exponents());
    if (arch::anyLaneSet(arch::greaterThanSigned(highest, ceilingLanes())) ||
        anyLoseLowBit(a, b)) {
        return false;
    }
    const Lanes bits =
        subtract ? arch::subtractFromFloat(a, b, acc) : arch::multiplyAddFloat(a, b, acc);
    if (!areLanesPlain(bits)) {
        return false;
    }
    bits.store(out);
    return true;
}

#else

inline void configureHostFloat() {}

inline std::optional<u32> add(u32, u32) { return std::nullopt; }
inline std::optional<u32> multiplyAdd(u32, u32, u32, bool) { return std::nullopt; }
inline std::optional<u32> sub(u32, u32) { return std::nullopt; }
inline std::optional<u32> mul(u32, u32) { return std::nullopt; }

inline bool addLanes(Lanes, Lanes, u32*) { return false; }
inline bool subLanes(Lanes, Lanes, u32*) { return false; }
inline bool mulLanes(Lanes, Lanes, u32*) { return false; }
inline bool maddLanes(Lanes, Lanes, Lanes, u32*, bool) { return false; }

#endif

}  // namespace uee::host
