#pragma once

#include "arch.h"
#include "compiler.h"
#include "single.h"
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

// The unit keeps one bit below the significand through the alignment shift and
// drops the rest. At 24 places that bit is the hidden one, so the exponent stays.
inline u32 getDroppedBits(u32 places) {
    return places == 0 ? 0u : places > 24 ? 31u : places - 1;
}

inline void alignOperands(u32& a, u32& b) {
    const s32 shift = static_cast<s32>(exponentOf(a)) - static_cast<s32>(exponentOf(b));
    const u32 places = static_cast<u32>(shift < 0 ? -shift : shift);
    const u32 drop = ~0u << getDroppedBits(places);
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

inline bool losesLowBit(u32 a, u32 b) {
    const u32 sb = 0x800000u | (b & 0x7FFFFFu);
    return single::canBorrowLowBit(sb, static_cast<u64>(0x800000u | (a & 0x7FFFFFu)) * sb);
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

inline Lanes getMagnitudes(Lanes value) {
    return arch::maskWith(value, Lanes::broadcast(0x7FFFFFFFu));
}

// The magnitude compares as an integer, so this is one test and not an
// exponent taken apart.
inline Lanes isAboveCeiling(Lanes value) {
    return arch::greaterThanSigned(getMagnitudes(value),
                                   Lanes::broadcast(((kHighestOperand + 1) << 23) - 1));
}

inline Lanes isNonzero(Lanes value) {
    return arch::greaterThanSigned(getMagnitudes(value), Lanes::broadcast(0x7FFFFF));
}

// Same as single::canBorrowLowBit. The product's low 23 bits come from the
// operands' low 23 bits alone, so the raw words multiply to them; a product
// the host flushed or that has a zero operand has nothing to borrow from.
inline Lanes getBorrowCandidates(Lanes a, Lanes b, Lanes product) {
    const Lanes zero = Lanes::broadcast(0);
    const Lanes quiet = arch::equalTo(
        arch::maskWith(arch::multiplyLowEach(a, b), Lanes::broadcast(0x7F8000)), zero);
    const Lanes unrecoded = arch::equalTo(arch::maskWith(b, Lanes::broadcast(0xFFFF)), zero);
    return arch::clearBits(arch::maskWith(quiet, isNonzero(product)), unrecoded);
}

// Each returns one bit per lane it answered, lane 0 lowest; the caller works out
// the rest in software. The vector unit keeps no overflow flag, so only a
// saturating result is refused.
inline u32 getAnswered(Lanes refused) {
    return ~arch::getSetLanes(refused) & 0xFu;
}

// The unit keeps one bit below the significand through the alignment and drops
// the rest. Same as getDroppedBits; a lane that does not shift is never masked,
// so its count does not matter.
inline Lanes alignAndAdd(Lanes a, Lanes b) {
    const Lanes shift = arch::subtractEach(a.exponents(), b.exponents());
    const Lanes places = arch::absoluteEach(shift);
    const Lanes guarded = arch::subtractEach(places, Lanes::broadcast(1));
    const Lanes clamped = arch::choose(guarded, Lanes::broadcast(31),
                                       arch::greaterThanSigned(places, Lanes::broadcast(24)));
    const Lanes drop = arch::shiftLeftEach(Lanes::broadcast(~0u), clamped);
    const Lanes keep = Lanes::broadcast(~0u);
    const Lanes zero = Lanes::broadcast(0);
    const Lanes alignedA =
        arch::maskWith(a, arch::choose(keep, drop, arch::greaterThanSigned(zero, shift)));
    const Lanes alignedB =
        arch::maskWith(b, arch::choose(keep, drop, arch::greaterThanSigned(shift, zero)));

    return arch::addFloat(alignedA, alignedB);
}

inline u32 addLanes(Lanes a, Lanes b, u32 out[4]) {
    const Lanes sum = alignAndAdd(a, b);
    sum.store(out);
    return getAnswered(
        arch::either(arch::either(isAboveCeiling(a), isAboveCeiling(b)), isAboveCeiling(sum)));
}

inline u32 subLanes(Lanes a, Lanes b, u32 out[4]) {
    return addLanes(a, arch::flipBits(b, Lanes::broadcast(kSignBit)), out);
}

struct BoothDigit {
    Lanes data;
    Lanes negate;
};

// single::computeBoothDigit for four lanes.
template <int place>
inline BoothDigit computeBoothDigit(Lanes a, Lanes b) {
    const Lanes zero = Lanes::broadcast(0);
    const Lanes three = Lanes::broadcast(3);
    const Lanes seven = Lanes::broadcast(7);
    Lanes test;
    if constexpr (place == 0) {
        test = arch::maskWith(arch::shiftLeftBy<1>(b), seven);
    } else {
        test = arch::maskWith(arch::shiftRightBy<place * 2 - 1>(b), seven);
    }
    Lanes value = arch::shiftLeftBy<place * 2>(a);
    const Lanes twice =
        arch::either(arch::equalTo(test, three), arch::equalTo(test, Lanes::broadcast(4)));
    value = arch::addEach(value, arch::maskWith(value, twice));
    const Lanes negate =
        arch::maskWith(arch::greaterThanSigned(test, three), arch::greaterThanSigned(seven, test));
    value = arch::flipBits(value, arch::maskWith(negate, Lanes::broadcast(~0u << (place * 2))));
    const Lanes used =
        arch::maskWith(arch::greaterThanSigned(test, zero), arch::greaterThanSigned(seven, test));
    return {arch::maskWith(value, used), arch::maskWith(negate, Lanes::broadcast(1u << (place * 2)))};
}

struct CarrySave {
    Lanes sum;
    Lanes carry;
};

inline CarrySave addThree(Lanes a, Lanes b, Lanes c) {
    const Lanes u = arch::flipBits(a, b);
    return {arch::flipBits(u, c),
            arch::shiftLeftBy<1>(arch::either(arch::maskWith(u, c), arch::maskWith(a, b)))};
}

// single::computeBoothProduct for four lanes of significands: the lanes whose
// tree borrows out of bit 15.
inline Lanes getBoothBorrows(Lanes a, Lanes b) {
    const BoothDigit d[8] = {computeBoothDigit<0>(a, b), computeBoothDigit<1>(a, b),
                             computeBoothDigit<2>(a, b), computeBoothDigit<3>(a, b),
                             computeBoothDigit<4>(a, b), computeBoothDigit<5>(a, b),
                             computeBoothDigit<6>(a, b), computeBoothDigit<7>(a, b)};
    const CarrySave t0 = addThree(d[1].data, d[2].data, d[3].data);
    CarrySave t1 = addThree(arch::clearBits(d[4].data, Lanes::broadcast(0x7FF)),
                            arch::clearBits(d[5].data, Lanes::broadcast(0xFFF)), d[6].data);
    t1.carry = arch::either(t1.carry, arch::either(d[6].negate,
                                                   arch::maskWith(d[5].data, Lanes::broadcast(0x800))));
    const Lanes d7 = arch::either(
        d[7].data,
        arch::addEach(arch::maskWith(d[5].data, Lanes::broadcast(0x400)), d[5].negate));
    const CarrySave t2 = addThree(d[0].data, t0.sum, t0.carry);
    const CarrySave t3 = addThree(d7, t1.sum, t1.carry);
    const CarrySave t4 = addThree(t2.carry, t3.sum, t3.carry);
    const CarrySave t5 = addThree(t2.sum, t4.sum, t4.carry);
    const Lanes high = Lanes::broadcast(0x7FFF);
    const Lanes low = arch::addEach(arch::clearBits(t5.sum, high),
                                    arch::clearBits(arch::addEach(t5.carry, d[7].negate), high));
    const Lanes bit = Lanes::broadcast(0x8000);
    return arch::equalTo(arch::maskWith(low, bit), bit);
}

// The truncated product is one below the host's where the tree borrows, except
// a carried product that had bit 23 to give. A borrow down to exponent zero is
// an underflow the host cannot report, so that lane is refused.
UEE_NOINLINE inline Lanes lowerBorrowedLanes(Lanes a, Lanes b, Lanes product, Lanes candidates,
                                             Lanes& refused) {
    const Lanes significand = Lanes::broadcast(0x7FFFFF);
    const Lanes implicit = Lanes::broadcast(0x800000);
    const Lanes sa = arch::either(arch::maskWith(a, significand), implicit);
    const Lanes sb = arch::either(arch::maskWith(b, significand), implicit);
    const Lanes zero = Lanes::broadcast(0);
    const Lanes carried = arch::equalTo(
        product.exponents(),
        arch::subtractEach(arch::addEach(a.exponents(), b.exponents()), Lanes::broadcast(126)));
    const Lanes bit23Clear = arch::equalTo(arch::maskWith(arch::multiplyLowEach(sa, sb), implicit), zero);
    const Lanes lowered = arch::maskWith(
        arch::maskWith(candidates, getBoothBorrows(sa, sb)),
        arch::maskWith(arch::flipBits(arch::equalTo(product.exponents(), zero), Lanes::broadcast(~0u)),
                       arch::either(arch::flipBits(carried, Lanes::broadcast(~0u)), bit23Clear)));
    const Lanes result = arch::subtractEach(product, arch::maskWith(lowered, Lanes::broadcast(1)));
    refused = arch::either(refused, arch::maskWith(lowered, arch::equalTo(result.exponents(), zero)));
    return result;
}

// itof: the conversion truncates, and scaling an integer down by at most 2^15
// only moves the exponent.
inline void convertFromIntLanes(const u32 in[4], u32 out[4], s32 fraction) {
    const Lanes value = arch::convertToFloat(Lanes(in));
    const Lanes nonzero =
        arch::flipBits(arch::equalTo(value, Lanes::broadcast(0)), Lanes::broadcast(~0u));
    const Lanes scale = Lanes::broadcast(static_cast<u32>(fraction) << 23);
    arch::subtractEach(value, arch::maskWith(scale, nonzero)).store(out);
}

// ftoi: a lane scaled to 2^31 or beyond saturates by its sign, and a zero
// exponent is a denormal the unit reads as zero.
inline void convertToIntLanes(const u32 in[4], u32 out[4], s32 fraction) {
    const Lanes value(in);
    const Lanes exponent = value.exponents();
    const Lanes zero = Lanes::broadcast(0);
    const Lanes isZero = arch::equalTo(exponent, zero);
    const Lanes saturates = arch::greaterThanSigned(
        arch::addEach(exponent, Lanes::broadcast(static_cast<u32>(fraction))),
        Lanes::broadcast(157));
    const Lanes scaled = arch::addEach(value, Lanes::broadcast(static_cast<u32>(fraction) << 23));
    const Lanes truncated = arch::maskWith(arch::truncateToInt(scaled),
                                           arch::flipBits(isZero, Lanes::broadcast(~0u)));
    const Lanes limit = arch::choose(Lanes::broadcast(0x7FFFFFFFu), Lanes::broadcast(0x80000000u),
                                     arch::greaterThanSigned(zero, value));
    arch::choose(truncated, limit, arch::clearBits(saturates, isZero)).store(out);
}

struct Answer {
    Lanes value;
    Lanes refused;
};

// Wanted names the lanes the caller keeps, lane 0 lowest; the rest may be wrong.
inline Answer multiplyLanes(Lanes a, Lanes b, u32 wanted) {
    Lanes refused = arch::either(isAboveCeiling(a), isAboveCeiling(b));
    Lanes product = arch::multiplyFloat(a, b);
    const Lanes candidates = getBorrowCandidates(a, b, product);
    if ((arch::getSetLanes(candidates) & wanted) != 0) {
        product = lowerBorrowedLanes(a, b, product, candidates, refused);
    }
    return {product, arch::either(refused, isAboveCeiling(product))};
}

inline u32 mulLanes(Lanes a, Lanes b, u32 out[4], u32 wanted) {
    const Answer product = multiplyLanes(a, b, wanted);
    product.value.store(out);
    return getAnswered(product.refused);
}

// One bit per lane, lane 0 lowest, for each of these.
struct ResultLanes {
    u32 zero;
    u32 largest;
    u32 sign;
};

inline ResultLanes getResultLanes(const u32 words[4]) {
    const Lanes value(words);
    const Lanes magnitude = arch::maskWith(value, Lanes::broadcast(single::kMagnitudeMask));
    return {arch::getSetLanes(arch::equalTo(magnitude, Lanes::broadcast(0))),
            arch::getSetLanes(arch::equalTo(magnitude, Lanes::broadcast(single::kVuLargest))),
            arch::getSetLanes(value)};
}

// Nothing a sum with an operand this large leaves can be too small to keep, so
// a zero it gives cancelled exactly.
constexpr u32 kClearOfUnderflow = 25;

inline u32 getClearOfUnderflow(const u32 words[4]) {
    return arch::getSetLanes(
        arch::greaterThanSigned(Lanes(words).exponents(), Lanes::broadcast(kClearOfUnderflow - 1)));
}

// A zero sum is exact when both operands were zero or both were large.
inline u32 getExactZeroSums(const u32 a[4], const u32 b[4]) {
    const Lanes zero = Lanes::broadcast(0);
    const u32 bothZero =
        arch::getSetLanes(arch::maskWith(arch::equalTo(Lanes(a).exponents(), zero),
                                         arch::equalTo(Lanes(b).exponents(), zero)));
    return bothZero | (getClearOfUnderflow(a) & getClearOfUnderflow(b));
}

// A product is zero when an operand is, and stays in range when the exponents
// sum to one the unit keeps; the rest are unsure.
struct ProductLanes {
    u32 zero;
    u32 sign;
    u32 unsure;
    u32 clearOfUnderflow;
};

inline ProductLanes getProductLanes(const u32 a[4], const u32 b[4]) {
    const Lanes x(a);
    const Lanes y(b);
    const Lanes zero = Lanes::broadcast(0);
    const Lanes isZero =
        arch::either(arch::equalTo(x.exponents(), zero), arch::equalTo(y.exponents(), zero));
    const Lanes sum = arch::addEach(x.exponents(), y.exponents());
    const Lanes inRange = arch::maskWith(arch::greaterThanSigned(sum, Lanes::broadcast(127)),
                                         arch::greaterThanSigned(Lanes::broadcast(382), sum));
    const Lanes clear =
        arch::greaterThanSigned(sum, Lanes::broadcast(127 + kClearOfUnderflow - 1));
    return {arch::getSetLanes(isZero), arch::getSetLanes(arch::flipBits(x, y)),
            ~arch::getSetLanes(arch::either(isZero, inRange)) & 0xFu,
            arch::getSetLanes(clear)};
}

// The product is truncated to the significand before the add, so this is two
// operations and not a fused one.
inline u32 maddLanes(Lanes acc, Lanes a, Lanes b, u32 out[4], bool subtract, u32 wanted) {
    const Answer product = multiplyLanes(a, b, wanted);
    // A product too small to keep still borrows from a small accumulator.
    const Lanes vanished =
        arch::clearBits(arch::maskWith(isNonzero(a), isNonzero(b)), isNonzero(product.value));
    const Lanes addend =
        subtract ? arch::flipBits(product.value, Lanes::broadcast(kSignBit)) : product.value;
    const Lanes sum = alignAndAdd(acc, addend);
    sum.store(out);
    // The addend's ceiling is the product's, which its refusal already holds.
    return getAnswered(arch::either(arch::either(product.refused, vanished),
                                    arch::either(isAboveCeiling(acc), isAboveCeiling(sum))));
}

#else

inline void configureHostFloat() {}

inline std::optional<u32> add(u32, u32) { return std::nullopt; }
inline std::optional<u32> multiplyAdd(u32, u32, u32, bool) { return std::nullopt; }
inline std::optional<u32> sub(u32, u32) { return std::nullopt; }
inline std::optional<u32> mul(u32, u32) { return std::nullopt; }

inline u32 addLanes(Lanes, Lanes, u32*) { return 0; }
inline u32 subLanes(Lanes, Lanes, u32*) { return 0; }
inline u32 mulLanes(Lanes, Lanes, u32*, u32) { return 0; }
inline void convertFromIntLanes(const u32*, u32*, s32) {}
inline void convertToIntLanes(const u32*, u32*, s32) {}
inline u32 maddLanes(Lanes, Lanes, Lanes, u32*, bool, u32) { return 0; }

struct ResultLanes {
    u32 zero;
    u32 largest;
    u32 sign;
};

struct ProductLanes {
    u32 zero;
    u32 sign;
    u32 unsure;
    u32 clearOfUnderflow;
};

// Every lane is special here, so every result takes the exact path.
inline ResultLanes getResultLanes(const u32*) { return {0, 0xFu, 0}; }
inline u32 getClearOfUnderflow(const u32*) { return 0; }
inline u32 getExactZeroSums(const u32*, const u32*) { return 0; }
inline ProductLanes getProductLanes(const u32*, const u32*) { return {0, 0, 0xFu, 0}; }

#endif

}  // namespace uee::host
