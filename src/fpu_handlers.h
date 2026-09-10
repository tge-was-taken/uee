#pragma once

#include "fpu.h"
#include "compiler.h"
#include "host_float.h"
#include "single.h"
#include "types.h"

namespace uee {

inline u32 causeOf(single::Rounded result) {
    return (result.overflow ? Fpu::kOverflow : 0u) | (result.underflow ? Fpu::kUnderflow : 0u);
}

// An operation that raises nothing leaves the cause register naming the last
// one that did.
inline void raiseIfAny(Fpu& fpu, u32 cause) {
    if (cause != 0) {
        fpu.raise(cause);
    }
}

inline u32 rounded(Fpu& fpu, single::Rounded result) {
    raiseIfAny(fpu, causeOf(result));
    return result.bits;
}

inline u32 flagged(Fpu& fpu, single::Rounded result, u32 cause) {
    raiseIfAny(fpu, cause);
    return result.bits;
}

// A denormal is nothing to the unit, so it divides by zero the same way.
inline bool isZeroValue(u32 bits) {
    return (single::flushDenormal(bits) & single::kMagnitudeMask) == 0;
}

inline u32 quotient(Fpu& fpu, u32 numerator, u32 denominator) {
    const single::Rounded result = single::div(numerator, denominator);
    if (!isZeroValue(denominator)) {
        return rounded(fpu, result);
    }
    return flagged(fpu, result,
                   isZeroValue(numerator) ? Fpu::kInvalid : Fpu::kDivideByZero);
}

// The multiply passes through the flag logic on its own before the sum
// replaces the visible result, and an accumulator that already saturated keeps
// reporting it.
UEE_NOINLINE inline u32 accumulatedSoft(Fpu& fpu, u32 a, u32 b, bool subtract) {
    single::Rounded result = single::multiplyAdd(fpu.acc, a, b, subtract);
    u32 cause = causeOf(result);
    if (fpu.accSaturated) {
        cause |= Fpu::kOverflow;
        result.bits = (result.bits & single::kSignMask) | single::kLargest;
    }
    // Unlike the plain operations, an accumulate names its own outcome in the
    // cause register even when there is nothing to report.
    fpu.raise(cause);
    // The multiply passes through the flag logic on its own, but only what it
    // leaves in the sticky copies survives the sum's own report.
    fpu.fcr31 |= causeOf(single::mul(a, b)) >> 11;
    return result.bits;
}

inline u32 accumulated(Fpu& fpu, u32 a, u32 b, bool subtract) {
    if (!fpu.accSaturated) {
        if (const std::optional<u32> fast = host::multiplyAdd(fpu.acc, a, b, subtract)) {
            return *fast;
        }
    }
    return accumulatedSoft(fpu, a, b, subtract);
}

inline u32 rootOf(Fpu& fpu, u32 value) {
    const single::Rounded root = single::sqrt(value);
    return (value & single::kSignMask) ? flagged(fpu, root, Fpu::kInvalid) : rounded(fpu, root);
}

// A root of nothing is only a division by zero if there is something to divide;
// nothing over nothing is the invalid one, as it is for the divider.
inline u32 reciprocalRootOf(Fpu& fpu, u32 numerator, u32 radicand) {
    const single::Rounded result = single::rsqrt(numerator, radicand);
    const bool rootIsZero = isZeroValue(radicand);
    const bool negative = (radicand & single::kSignMask) != 0;
    if (!rootIsZero && !negative) {
        return rounded(fpu, result);
    }
    const bool zeroNumerator = isZeroValue(numerator);
    u32 cause = (negative || zeroNumerator) ? Fpu::kInvalid : 0u;
    if (rootIsZero && !zeroNumerator) {
        cause |= Fpu::kDivideByZero;
    }
    return flagged(fpu, result, cause);
}

// The host answers only where it raises no flag, so taking its answer leaves
// the cause register alone. It answers 98%, so the check is inlined.
UEE_NOINLINE inline u32 addedSoft(Fpu& fpu, u32 a, u32 b) {
    return rounded(fpu, single::add(a, b));
}
UEE_NOINLINE inline u32 subtractedSoft(Fpu& fpu, u32 a, u32 b) {
    return rounded(fpu, single::sub(a, b));
}
UEE_NOINLINE inline u32 multipliedSoft(Fpu& fpu, u32 a, u32 b) {
    return rounded(fpu, single::mul(a, b));
}

inline u32 added(Fpu& fpu, u32 a, u32 b) {
    if (const std::optional<u32> fast = host::add(a, b)) {
        return *fast;
    }
    return addedSoft(fpu, a, b);
}
inline u32 subtracted(Fpu& fpu, u32 a, u32 b) {
    if (const std::optional<u32> fast = host::sub(a, b)) {
        return *fast;
    }
    return subtractedSoft(fpu, a, b);
}
inline u32 multiplied(Fpu& fpu, u32 a, u32 b) {
    if (const std::optional<u32> fast = host::mul(a, b)) {
        return *fast;
    }
    return multipliedSoft(fpu, a, b);
}
UEE_NOINLINE inline void div_s(Fpu& fpu, u32 fd, u32 fs, u32 ft) {
    fpu.fpr[fd] = quotient(fpu, fpu.fpr[fs], fpu.fpr[ft]);
}

inline void add_s(Fpu& fpu, u32 fd, u32 fs, u32 ft) {
    fpu.fpr[fd] = added(fpu, fpu.fpr[fs], fpu.fpr[ft]);
}
inline void sub_s(Fpu& fpu, u32 fd, u32 fs, u32 ft) {
    fpu.fpr[fd] = subtracted(fpu, fpu.fpr[fs], fpu.fpr[ft]);
}
inline void mul_s(Fpu& fpu, u32 fd, u32 fs, u32 ft) {
    fpu.fpr[fd] = multiplied(fpu, fpu.fpr[fs], fpu.fpr[ft]);
}
// A saturated result is the only one the accumulator has to remember, and the
// host answers only where nothing saturated.
inline void writeAcc(Fpu& fpu, u32 value, bool saturated) {
    fpu.acc = value;
    fpu.accSaturated = saturated;
}

UEE_NOINLINE inline void adda_s(Fpu& fpu, u32 fs, u32 ft) {
    const u32 a = fpu.fpr[fs], b = fpu.fpr[ft];
    if (const std::optional<u32> fast = host::add(a, b)) {
        writeAcc(fpu, *fast, false);
        return;
    }
    const single::Rounded result = single::add(a, b);
    raiseIfAny(fpu, causeOf(result));
    writeAcc(fpu, result.bits, result.overflow);
}
UEE_NOINLINE inline void suba_s(Fpu& fpu, u32 fs, u32 ft) {
    const u32 a = fpu.fpr[fs], b = fpu.fpr[ft];
    if (const std::optional<u32> fast = host::sub(a, b)) {
        writeAcc(fpu, *fast, false);
        return;
    }
    const single::Rounded result = single::sub(a, b);
    raiseIfAny(fpu, causeOf(result));
    writeAcc(fpu, result.bits, result.overflow);
}
UEE_NOINLINE inline void mula_s(Fpu& fpu, u32 fs, u32 ft) {
    const u32 a = fpu.fpr[fs], b = fpu.fpr[ft];
    if (const std::optional<u32> fast = host::mul(a, b)) {
        writeAcc(fpu, *fast, false);
        return;
    }
    const single::Rounded result = single::mul(a, b);
    raiseIfAny(fpu, causeOf(result));
    writeAcc(fpu, result.bits, result.overflow);
}

UEE_NOINLINE inline void madd_s(Fpu& fpu, u32 fd, u32 fs, u32 ft) {
    fpu.fpr[fd] = accumulated(fpu, fpu.fpr[fs], fpu.fpr[ft], false);
}
UEE_NOINLINE inline void msub_s(Fpu& fpu, u32 fd, u32 fs, u32 ft) {
    fpu.fpr[fd] = accumulated(fpu, fpu.fpr[fs], fpu.fpr[ft], true);
}
UEE_NOINLINE inline void madda_s(Fpu& fpu, u32 fs, u32 ft) {
    const u32 value = accumulated(fpu, fpu.fpr[fs], fpu.fpr[ft], false);
    writeAcc(fpu, value, (fpu.fcr31 & Fpu::kOverflow) != 0);
}
UEE_NOINLINE inline void msuba_s(Fpu& fpu, u32 fs, u32 ft) {
    const u32 value = accumulated(fpu, fpu.fpr[fs], fpu.fpr[ft], true);
    writeAcc(fpu, value, (fpu.fcr31 & Fpu::kOverflow) != 0);
}

UEE_NOINLINE inline void sqrt_s(Fpu& fpu, u32 fd, u32 ft) {
    fpu.fpr[fd] = rootOf(fpu, fpu.fpr[ft]);
}
UEE_NOINLINE inline void rsqrt_s(Fpu& fpu, u32 fd, u32 fs, u32 ft) {
    fpu.fpr[fd] = reciprocalRootOf(fpu, fpu.fpr[fs], fpu.fpr[ft]);
}

inline void abs_s(Fpu& fpu, u32 fd, u32 fs) {
    fpu.fpr[fd] = fpu.fpr[fs] & ~single::kSignMask;
}
inline void neg_s(Fpu& fpu, u32 fd, u32 fs) {
    fpu.fpr[fd] = fpu.fpr[fs] ^ single::kSignMask;
}
inline void mov_s(Fpu& fpu, u32 fd, u32 fs) {
    fpu.fpr[fd] = fpu.fpr[fs];
}
inline void max_s(Fpu& fpu, u32 fd, u32 fs, u32 ft) {
    const u32 a = fpu.fpr[fs], b = fpu.fpr[ft];
    fpu.fpr[fd] = single::signedOrderKey(a) < single::signedOrderKey(b) ? b : a;
}
inline void min_s(Fpu& fpu, u32 fd, u32 fs, u32 ft) {
    const u32 a = fpu.fpr[fs], b = fpu.fpr[ft];
    fpu.fpr[fd] = single::signedOrderKey(b) < single::signedOrderKey(a) ? b : a;
}

UEE_NOINLINE inline void cvt_w_s(Fpu& fpu, u32 fd, u32 fs) {
    fpu.fpr[fd] = static_cast<u32>(single::toInt32(fpu.fpr[fs]));
}
UEE_NOINLINE inline void cvt_s_w(Fpu& fpu, u32 fd, u32 fs) {
    fpu.fpr[fd] = single::fromInt32(static_cast<s32>(fpu.fpr[fs]));
}

// Leaves the last arithmetic operation's cause bits standing.
inline void setCompare(Fpu& fpu, bool set) {
    fpu.fcr31 = set ? (fpu.fcr31 | Fpu::kCompareBit) : (fpu.fcr31 & ~Fpu::kCompareBit);
}

// Nothing in the format is unordered, so the four conditions the R5900
// implements collapse to a total order.
inline void c_f_s(Fpu& fpu) {
    setCompare(fpu, false);
}
inline void c_eq_s(Fpu& fpu, u32 fs, u32 ft) {
    setCompare(fpu, single::orderKey(fpu.fpr[fs]) == single::orderKey(fpu.fpr[ft]));
}
inline void c_lt_s(Fpu& fpu, u32 fs, u32 ft) {
    setCompare(fpu, single::orderKey(fpu.fpr[fs]) < single::orderKey(fpu.fpr[ft]));
}
inline void c_le_s(Fpu& fpu, u32 fs, u32 ft) {
    setCompare(fpu, single::orderKey(fpu.fpr[fs]) <= single::orderKey(fpu.fpr[ft]));
}

}  // namespace uee
