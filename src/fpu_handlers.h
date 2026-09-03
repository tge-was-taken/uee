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

inline u32 rounded(Fpu& fpu, single::Rounded result) {
    fpu.raise(causeOf(result));
    return result.bits;
}

inline u32 flagged(Fpu& fpu, single::Rounded result, u32 cause) {
    fpu.raise(cause);
    return result.bits;
}

inline u32 quotient(Fpu& fpu, u32 numerator, u32 denominator) {
    const single::Rounded result = single::div(numerator, denominator);
    if ((denominator & single::kMagnitudeMask) != 0) {
        return rounded(fpu, result);
    }
    const bool zeroByZero = (numerator & single::kMagnitudeMask) == 0;
    return flagged(fpu, result, zeroByZero ? Fpu::kInvalid : Fpu::kDivideByZero);
}

UEE_NOINLINE inline u32 accumulatedSoft(Fpu& fpu, u32 a, u32 b, bool subtract) {
    return rounded(fpu, single::multiplyAdd(fpu.acc, a, b, subtract));
}

inline u32 accumulated(Fpu& fpu, u32 a, u32 b, bool subtract) {
    if (const std::optional<u32> fast = host::multiplyAdd(fpu.acc, a, b, subtract)) {
        return *fast;
    }
    return accumulatedSoft(fpu, a, b, subtract);
}

inline u32 rootOf(Fpu& fpu, u32 value) {
    const single::Rounded root = single::sqrt(value);
    return (value & single::kSignMask) ? flagged(fpu, root, Fpu::kInvalid) : rounded(fpu, root);
}

inline u32 reciprocalRootOf(Fpu& fpu, u32 numerator, u32 radicand) {
    const single::Rounded result = single::rsqrt(numerator, radicand);
    if (radicand & single::kSignMask) {
        return flagged(fpu, result, Fpu::kInvalid);
    }
    if ((radicand & single::kMagnitudeMask) == 0) {
        return flagged(fpu, result, Fpu::kDivideByZero);
    }
    return rounded(fpu, result);
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
UEE_NOINLINE inline void adda_s(Fpu& fpu, u32 fs, u32 ft) {
    fpu.acc = added(fpu, fpu.fpr[fs], fpu.fpr[ft]);
}
UEE_NOINLINE inline void suba_s(Fpu& fpu, u32 fs, u32 ft) {
    fpu.acc = subtracted(fpu, fpu.fpr[fs], fpu.fpr[ft]);
}
UEE_NOINLINE inline void mula_s(Fpu& fpu, u32 fs, u32 ft) {
    fpu.acc = multiplied(fpu, fpu.fpr[fs], fpu.fpr[ft]);
}

UEE_NOINLINE inline void madd_s(Fpu& fpu, u32 fd, u32 fs, u32 ft) {
    fpu.fpr[fd] = accumulated(fpu, fpu.fpr[fs], fpu.fpr[ft], false);
}
UEE_NOINLINE inline void msub_s(Fpu& fpu, u32 fd, u32 fs, u32 ft) {
    fpu.fpr[fd] = accumulated(fpu, fpu.fpr[fs], fpu.fpr[ft], true);
}
UEE_NOINLINE inline void madda_s(Fpu& fpu, u32 fs, u32 ft) {
    fpu.acc = accumulated(fpu, fpu.fpr[fs], fpu.fpr[ft], false);
}
UEE_NOINLINE inline void msuba_s(Fpu& fpu, u32 fs, u32 ft) {
    fpu.acc = accumulated(fpu, fpu.fpr[fs], fpu.fpr[ft], true);
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
