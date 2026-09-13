#pragma once

#include "fpu.h"
#include "compiler.h"
#include "host_float.h"
#include "single.h"
#include "types.h"

namespace uee {

inline u32 getCause(single::Rounded result) {
    return (result.overflow ? Fpu::kOverflow : 0u) | (result.underflow ? Fpu::kUnderflow : 0u);
}

// An operation that raises nothing leaves the cause register naming the last
// one that did.
inline void raiseIfAny(Fpu& fpu, u32 cause) {
    if (cause != 0) {
        fpu.raise(cause);
    }
}

inline u32 takeResult(Fpu& fpu, single::Rounded result, u32 cause) {
    raiseIfAny(fpu, cause);
    return result.bits;
}

inline u32 takeResult(Fpu& fpu, single::Rounded result) {
    return takeResult(fpu, result, getCause(result));
}

// A denormal is nothing to the unit, so it divides by zero the same way.
inline bool isZeroValue(u32 bits) {
    return (single::flushDenormal(bits) & single::kMagnitudeMask) == 0;
}

inline u32 divide(Fpu& fpu, u32 numerator, u32 denominator) {
    const single::Rounded result = single::div(numerator, denominator);
    if (!isZeroValue(denominator)) {
        return takeResult(fpu, result);
    }
    return takeResult(fpu, result, isZeroValue(numerator) ? Fpu::kInvalid : Fpu::kDivideByZero);
}

// An accumulator that already saturated keeps reporting it.
UEE_NOINLINE inline u32 multiplyAccumulateSoft(Fpu& fpu, u32 a, u32 b, bool subtract) {
    single::Rounded result = single::multiplyAdd(fpu.acc, a, b, subtract);
    u32 cause = getCause(result);
    if (fpu.accSaturated) {
        cause |= Fpu::kOverflow;
        result.bits = (result.bits & single::kSignMask) | single::kLargest;
    }
    // Unlike the plain operations, an accumulate names its own outcome in the
    // cause register even when there is nothing to report.
    fpu.raise(cause);
    // The multiply passes through the flag logic on its own, but only what it
    // leaves in the sticky copies survives the sum's own report.
    fpu.fcr31 |= getCause(single::mul(a, b)) >> 11;
    return result.bits;
}

inline u32 multiplyAccumulate(Fpu& fpu, u32 a, u32 b, bool subtract) {
    if (!fpu.accSaturated) {
        if (const std::optional<u32> fast = host::multiplyAdd(fpu.acc, a, b, subtract)) {
            fpu.raise(0);
            return *fast;
        }
    }
    return multiplyAccumulateSoft(fpu, a, b, subtract);
}

inline u32 takeSquareRoot(Fpu& fpu, u32 value) {
    const single::Rounded root = single::sqrt(value);
    return (value & single::kSignMask) ? takeResult(fpu, root, Fpu::kInvalid)
                                       : takeResult(fpu, root);
}

// A root of nothing is only a division by zero if there is something to divide;
// nothing over nothing is the invalid one, as it is for the divider.
inline u32 takeReciprocalRoot(Fpu& fpu, u32 numerator, u32 radicand) {
    const single::Rounded result = single::rsqrt(numerator, radicand);
    const bool rootIsZero = isZeroValue(radicand);
    const bool negative = (radicand & single::kSignMask) != 0;
    if (!rootIsZero && !negative) {
        return takeResult(fpu, result);
    }
    const bool zeroNumerator = isZeroValue(numerator);
    u32 cause = (negative || zeroNumerator) ? Fpu::kInvalid : 0u;
    if (rootIsZero && !zeroNumerator) {
        cause |= Fpu::kDivideByZero;
    }
    return takeResult(fpu, result, cause);
}

using HostOperation = std::optional<u32> (*)(u32, u32);
using ExactOperation = single::Rounded (*)(u32, u32);

// The host answers only where it raises no flag, so taking its answer leaves
// the cause register alone. It answers 98%, so the check is inlined.
template <ExactOperation exact>
UEE_NOINLINE inline u32 computeSoft(Fpu& fpu, u32 a, u32 b) {
    return takeResult(fpu, exact(a, b));
}

template <HostOperation host, ExactOperation exact>
inline u32 compute(Fpu& fpu, u32 a, u32 b) {
    if (const std::optional<u32> fast = host(a, b)) {
        return *fast;
    }
    return computeSoft<exact>(fpu, a, b);
}

UEE_NOINLINE inline void div_s(Fpu& fpu, u32 fd, u32 fs, u32 ft) {
    fpu.fpr[fd] = divide(fpu, fpu.fpr[fs], fpu.fpr[ft]);
}

inline void add_s(Fpu& fpu, u32 fd, u32 fs, u32 ft) {
    fpu.fpr[fd] = compute<host::add, single::add>(fpu, fpu.fpr[fs], fpu.fpr[ft]);
}
inline void sub_s(Fpu& fpu, u32 fd, u32 fs, u32 ft) {
    fpu.fpr[fd] = compute<host::sub, single::sub>(fpu, fpu.fpr[fs], fpu.fpr[ft]);
}
inline void mul_s(Fpu& fpu, u32 fd, u32 fs, u32 ft) {
    fpu.fpr[fd] = compute<host::mul, single::mul>(fpu, fpu.fpr[fs], fpu.fpr[ft]);
}

// A saturated result is the only one the accumulator has to remember, and the
// host answers only where nothing saturated.
inline void writeAcc(Fpu& fpu, u32 value, bool saturated) {
    fpu.acc = value;
    fpu.accSaturated = saturated;
}

template <HostOperation host, ExactOperation exact>
UEE_NOINLINE inline void computeIntoAcc(Fpu& fpu, u32 a, u32 b) {
    if (const std::optional<u32> fast = host(a, b)) {
        writeAcc(fpu, *fast, false);
        return;
    }
    const single::Rounded result = exact(a, b);
    raiseIfAny(fpu, getCause(result));
    writeAcc(fpu, result.bits, result.overflow);
}

inline void adda_s(Fpu& fpu, u32 fs, u32 ft) {
    computeIntoAcc<host::add, single::add>(fpu, fpu.fpr[fs], fpu.fpr[ft]);
}
inline void suba_s(Fpu& fpu, u32 fs, u32 ft) {
    computeIntoAcc<host::sub, single::sub>(fpu, fpu.fpr[fs], fpu.fpr[ft]);
}
inline void mula_s(Fpu& fpu, u32 fs, u32 ft) {
    computeIntoAcc<host::mul, single::mul>(fpu, fpu.fpr[fs], fpu.fpr[ft]);
}

UEE_NOINLINE inline void madd_s(Fpu& fpu, u32 fd, u32 fs, u32 ft) {
    fpu.fpr[fd] = multiplyAccumulate(fpu, fpu.fpr[fs], fpu.fpr[ft], false);
}
UEE_NOINLINE inline void msub_s(Fpu& fpu, u32 fd, u32 fs, u32 ft) {
    fpu.fpr[fd] = multiplyAccumulate(fpu, fpu.fpr[fs], fpu.fpr[ft], true);
}
UEE_NOINLINE inline void madda_s(Fpu& fpu, u32 fs, u32 ft) {
    const u32 value = multiplyAccumulate(fpu, fpu.fpr[fs], fpu.fpr[ft], false);
    writeAcc(fpu, value, (fpu.fcr31 & Fpu::kOverflow) != 0);
}
UEE_NOINLINE inline void msuba_s(Fpu& fpu, u32 fs, u32 ft) {
    const u32 value = multiplyAccumulate(fpu, fpu.fpr[fs], fpu.fpr[ft], true);
    writeAcc(fpu, value, (fpu.fcr31 & Fpu::kOverflow) != 0);
}

UEE_NOINLINE inline void sqrt_s(Fpu& fpu, u32 fd, u32 ft) {
    fpu.fpr[fd] = takeSquareRoot(fpu, fpu.fpr[ft]);
}
UEE_NOINLINE inline void rsqrt_s(Fpu& fpu, u32 fd, u32 fs, u32 ft) {
    fpu.fpr[fd] = takeReciprocalRoot(fpu, fpu.fpr[fs], fpu.fpr[ft]);
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
    fpu.fpr[fd] = single::getSignedOrderKey(a) < single::getSignedOrderKey(b) ? b : a;
}
inline void min_s(Fpu& fpu, u32 fd, u32 fs, u32 ft) {
    const u32 a = fpu.fpr[fs], b = fpu.fpr[ft];
    fpu.fpr[fd] = single::getSignedOrderKey(b) < single::getSignedOrderKey(a) ? b : a;
}

UEE_NOINLINE inline void cvt_w_s(Fpu& fpu, u32 fd, u32 fs) {
    fpu.fpr[fd] = static_cast<u32>(single::convertToInt32(fpu.fpr[fs]));
}
UEE_NOINLINE inline void cvt_s_w(Fpu& fpu, u32 fd, u32 fs) {
    fpu.fpr[fd] = single::convertFromInt32(static_cast<s32>(fpu.fpr[fs]));
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
    setCompare(fpu, single::getOrderKey(fpu.fpr[fs]) == single::getOrderKey(fpu.fpr[ft]));
}
inline void c_lt_s(Fpu& fpu, u32 fs, u32 ft) {
    setCompare(fpu, single::getOrderKey(fpu.fpr[fs]) < single::getOrderKey(fpu.fpr[ft]));
}
inline void c_le_s(Fpu& fpu, u32 fs, u32 ft) {
    setCompare(fpu, single::getOrderKey(fpu.fpr[fs]) <= single::getOrderKey(fpu.fpr[ft]));
}

}  // namespace uee
