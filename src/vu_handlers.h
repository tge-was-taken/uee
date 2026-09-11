#pragma once

#include "memory.h"
#include "opcodes.h"
#include "single.h"
#include "types.h"
#include "compiler.h"
#include "host_float.h"
#include "vu.h"
#include "vu_micro.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace uee {

enum class VuKind { Add, Sub, Mul, Max, Mini, Madd, Msub };
enum class VuSource { Vt, X, Y, Z, W, Q, I };

struct VuForm {
    VuKind kind;
    VuSource source;
    bool toAcc;
};

// An operand above the range is brought down; a saturated result is not.
inline u32 clampVuResult(single::Rounded rounded) {
    return rounded.overflow ? rounded.bits : single::clampToVu(rounded.bits);
}

inline u32 getRangeFlags(single::Rounded rounded) {
    return (rounded.underflow ? Vu::kUnderflow : 0u) | (rounded.overflow ? Vu::kOverflow : 0u);
}

inline u32 readOperand(const Vu& vu0, VuSource source, const Reg128& vt, s32 i) {
    switch (source) {
        case VuSource::X: return vt.word(0);
        case VuSource::Y: return vt.word(1);
        case VuSource::Z: return vt.word(2);
        case VuSource::W: return vt.word(3);
        case VuSource::Q: return vu0.vi[Vu::kQ];
        case VuSource::I: return vu0.vi[Vu::kI];
        default: return vt.word(i);
    }
}

// The adder loses whatever shifts past its width.
template <VuKind kind>
inline u32 combineLane(const Vu& vu0, u32 a, u32 b, s32 i) {
    if constexpr (kind == VuKind::Add) {
        return clampVuResult(single::add(a, b));
    } else if constexpr (kind == VuKind::Sub) {
        return clampVuResult(single::sub(a, b));
    } else if constexpr (kind == VuKind::Mul) {
        return clampVuResult(single::mul(a, b));
    } else if constexpr (kind == VuKind::Max) {
        // Nothing is computed, so the operand is passed on whole.
        return single::getVuOrderKey(a) > single::getVuOrderKey(b) ? a : b;
    } else if constexpr (kind == VuKind::Mini) {
        return single::getVuOrderKey(a) < single::getVuOrderKey(b) ? a : b;
    } else {
        return clampVuResult(
            single::multiplyAdd(vu0.acc.word(i), a, b, kind == VuKind::Msub));
    }
}

// The four-lane path reports neither the range nor the exponent the accumulator
// keeps, so a result that saturated or vanished is worked out again here.
template <VuKind kind>
inline single::Parts computeExactLane(const Vu& vu0, u32 a, u32 b, s32 i) {
    if constexpr (kind == VuKind::Add) {
        return single::addToParts(single::split(a), single::split(b));
    } else if constexpr (kind == VuKind::Sub) {
        single::Parts negated = single::split(b);
        negated.sign ^= single::kSignMask;
        return single::addToParts(single::split(a), negated);
    } else if constexpr (kind == VuKind::Mul) {
        return single::multiplyParts(single::split(a), single::split(b));
    } else {
        single::Parts product = single::multiplyParts(single::split(a), single::split(b));
        if constexpr (kind == VuKind::Msub) {
            product.sign ^= single::kSignMask;
        }
        return single::addToParts(single::split(vu0.acc.word(i)), product);
    }
}

// The result alone cannot say whether it saturated or an operand was already
// that large, nor a true zero from one too small to keep.
inline bool needsExactLane(u32 result) {
    const u32 magnitude = result & single::kMagnitudeMask;
    return magnitude == 0 || magnitude == single::kVuLargest;
}

// False says the host unit could not answer for all four lanes. The two that
// pick an operand rather than compute one are not worth handing over.
template <VuKind kind>
inline bool combineLanes(const Vu& vu0, const Reg128& a, const Reg128& b, u32 out[4]) {
    if constexpr (!host::kHasFloatUnit || kind == VuKind::Max || kind == VuKind::Mini) {
        return false;
    } else {
        static_assert(sizeof(Reg128) == 4 * sizeof(u32),
                      "the lanes are read straight out of the register");
        const host::Lanes x{reinterpret_cast<const u32*>(&a)};
        const host::Lanes y{reinterpret_cast<const u32*>(&b)};
        if constexpr (kind == VuKind::Add) {
            return host::addLanes(x, y, out);
        } else if constexpr (kind == VuKind::Sub) {
            return host::subLanes(x, y, out);
        } else if constexpr (kind == VuKind::Mul) {
            return host::mulLanes(x, y, out);
        } else {
            const host::Lanes acc{reinterpret_cast<const u32*>(&vu0.acc)};
            return host::maddLanes(acc, x, y, out, kind == VuKind::Msub);
        }
    }
}

// Shared, with the lanes and broadcast settled at each call site.
template <VuKind kind>
UEE_NOINLINE inline void computeLanes(const Vu& vu0, const Reg128& a, const Reg128& b,
                                      u32 out[4]) {
    if (combineLanes<kind>(vu0, a, b, out)) {
        return;
    }
    for (s32 i = 0; i < 4; ++i) {
        out[i] = combineLane<kind>(vu0, a.word(i), b.word(i), i);
    }
}

// The kind is fixed per call, so four lanes run as one operation, not a switch.
template <VuKind kind>
inline void vectorOp(Vu& vu0, VuSource source, bool toAcc, u32 dest, u32 vd, u32 vs,
                     u32 vt) {
    const Reg128 a = vu0.readVf(vs);
    const Reg128 read = vu0.readVf(vt);
    const u32 shared = readOperand(vu0, source, read, 0);
    const Reg128 b =
        source == VuSource::Vt ? read : lanes(shared, shared, shared, shared);
    u32 out[4];
    computeLanes<kind>(vu0, a, b, out);
    // The two that pick an operand rather than compute one leave them alone.
    if constexpr (kind == VuKind::Max || kind == VuKind::Mini) {
        vu0.writeVf(vd, dest, out);
        return;
    } else {
        if constexpr (kind == VuKind::Madd || kind == VuKind::Msub) {
            u32 product[4];
            u32 productFlags[4];
            for (s32 i = 0; i < 4; ++i) {
                const single::Rounded rounded = single::mul(a.word(i), b.word(i));
                product[i] = clampVuResult(rounded);
                productFlags[i] = getRangeFlags(rounded);
            }
            vu0.addSticky(dest, product, productFlags);
        }

        constexpr bool readsAcc = kind == VuKind::Madd || kind == VuKind::Msub;
        u32 flags[4];
        bool saturated[4];
        for (s32 i = 0; i < 4; ++i) {
            flags[i] = 0;
            if (needsExactLane(out[i])) {
                flags[i] = getRangeFlags(
                    single::join(computeExactLane<kind>(vu0, a.word(i), b.word(i), i)));
            }
            if constexpr (readsAcc) {
                if (vu0.accSaturated[i]) {
                    flags[i] |= Vu::kOverflow;
                }
            }
            saturated[i] = (flags[i] & Vu::kOverflow) != 0;
            if (saturated[i]) {
                out[i] = (out[i] & single::kSignMask) | single::kLargest;
            }
        }
        vu0.setFlags(dest, out, flags);

        if (toAcc) {
            vu0.writeAcc(dest, out, saturated);
        } else {
            vu0.writeVf(vd, dest, out);
        }
    }
}

// The interpreter reads the kind out of the encoding, where emitted code names
// it outright.
inline void vectorOp(Vu& vu0, VuForm form, u32 dest, u32 vd, u32 vs, u32 vt) {
    switch (form.kind) {
        case VuKind::Add:
            vectorOp<VuKind::Add>(vu0, form.source, form.toAcc, dest, vd, vs, vt);
            return;
        case VuKind::Sub:
            vectorOp<VuKind::Sub>(vu0, form.source, form.toAcc, dest, vd, vs, vt);
            return;
        case VuKind::Mul:
            vectorOp<VuKind::Mul>(vu0, form.source, form.toAcc, dest, vd, vs, vt);
            return;
        case VuKind::Max:
            vectorOp<VuKind::Max>(vu0, form.source, form.toAcc, dest, vd, vs, vt);
            return;
        case VuKind::Mini:
            vectorOp<VuKind::Mini>(vu0, form.source, form.toAcc, dest, vd, vs, vt);
            return;
        case VuKind::Madd:
            vectorOp<VuKind::Madd>(vu0, form.source, form.toAcc, dest, vd, vs, vt);
            return;
        case VuKind::Msub:
            vectorOp<VuKind::Msub>(vu0, form.source, form.toAcc, dest, vd, vs, vt);
            return;
    }
}

inline void vadd(Vu& vu0, VuSource from, u32 dest, u32 vd, u32 vs, u32 vt) {
    vectorOp<VuKind::Add>(vu0, from, false, dest, vd, vs, vt);
}
inline void vsub(Vu& vu0, VuSource from, u32 dest, u32 vd, u32 vs, u32 vt) {
    vectorOp<VuKind::Sub>(vu0, from, false, dest, vd, vs, vt);
}
inline void vmul(Vu& vu0, VuSource from, u32 dest, u32 vd, u32 vs, u32 vt) {
    vectorOp<VuKind::Mul>(vu0, from, false, dest, vd, vs, vt);
}
inline void vmax(Vu& vu0, VuSource from, u32 dest, u32 vd, u32 vs, u32 vt) {
    vectorOp<VuKind::Max>(vu0, from, false, dest, vd, vs, vt);
}
inline void vmini(Vu& vu0, VuSource from, u32 dest, u32 vd, u32 vs, u32 vt) {
    vectorOp<VuKind::Mini>(vu0, from, false, dest, vd, vs, vt);
}
inline void vmadd(Vu& vu0, VuSource from, u32 dest, u32 vd, u32 vs, u32 vt) {
    vectorOp<VuKind::Madd>(vu0, from, false, dest, vd, vs, vt);
}
inline void vmsub(Vu& vu0, VuSource from, u32 dest, u32 vd, u32 vs, u32 vt) {
    vectorOp<VuKind::Msub>(vu0, from, false, dest, vd, vs, vt);
}

inline void vadda(Vu& vu0, VuSource from, u32 dest, u32 vs, u32 vt) {
    vectorOp<VuKind::Add>(vu0, from, true, dest, 0, vs, vt);
}
inline void vsuba(Vu& vu0, VuSource from, u32 dest, u32 vs, u32 vt) {
    vectorOp<VuKind::Sub>(vu0, from, true, dest, 0, vs, vt);
}
inline void vmula(Vu& vu0, VuSource from, u32 dest, u32 vs, u32 vt) {
    vectorOp<VuKind::Mul>(vu0, from, true, dest, 0, vs, vt);
}
inline void vmadda(Vu& vu0, VuSource from, u32 dest, u32 vs, u32 vt) {
    vectorOp<VuKind::Madd>(vu0, from, true, dest, 0, vs, vt);
}
inline void vmsuba(Vu& vu0, VuSource from, u32 dest, u32 vs, u32 vt) {
    vectorOp<VuKind::Msub>(vu0, from, true, dest, 0, vs, vt);
}

template <typename F>
inline void vectorUnary(Vu& vu0, u32 dest, u32 vt, u32 vs, F transform) {
    const Reg128 source = vu0.readVf(vs);
    u32 out[4];
    for (s32 i = 0; i < 4; ++i) {
        out[i] = transform(source.word(i));
    }
    vu0.writeVf(vt, dest, out);
}

inline void vbroadcast(Vu& vu0, u32 dest, u32 vt, u32 value) {
    const u32 lanes[4] = {value, value, value, value};
    vu0.writeVf(vt, dest, lanes);
}

// The integer file is sixteen bits and the float lane is thirty two, so the
// move carries the sign up.
inline void vmfir(Vu& vu0, u32 dest, u32 vt, u32 vis) {
    const u32 value = vu0.readVi(vis);
    vbroadcast(vu0, dest, vt, static_cast<u32>(static_cast<s32>(value << 16) >> 16));
}

inline void vmfp(Vu& vu0, u32 dest, u32 vt) {
    vbroadcast(vu0, dest, vt, vu0.vi[Vu::kP]);
}

inline void vmtir(Vu& vu0, u32 vit, u32 vfs, s32 lane) {
    vu0.writeVi(vit, vu0.readVf(vfs).word(lane));
}

// R keeps a 23-bit mantissa and reads back with the exponent the unit fixes
// for it.
constexpr u32 kRandomMantissa = 0x007FFFFFu;
constexpr u32 kRandomExponent = 0x3F800000u;

inline void vrinit(Vu& vu0, u32 value) { vu0.vi[Vu::kR] = value & kRandomMantissa; }

inline void vrxor(Vu& vu0, u32 value) { vu0.vi[Vu::kR] ^= value & kRandomMantissa; }

inline void vrget(Vu& vu0, u32 dest, u32 vt) {
    vbroadcast(vu0, dest, vt, kRandomExponent | (vu0.vi[Vu::kR] & kRandomMantissa));
}

inline void vmove(Vu& vu0, u32 dest, u32 vt, u32 vs) {
    vectorUnary(vu0, dest, vt, vs, [](u32 word) { return word; });
}
// Every field takes the one above it and the top wraps round to the bottom.
// The source is read whole first, so rotating a register onto itself works.
inline void vmr32(Vu& vu0, u32 dest, u32 vt, u32 vs) {
    const Reg128 source = vu0.readVf(vs);
    const u32 out[4] = {source.word(1), source.word(2), source.word(3), source.word(0)};
    vu0.writeVf(vt, dest, out);
}
inline void vabs(Vu& vu0, u32 dest, u32 vt, u32 vs) {
    vectorUnary(vu0, dest, vt, vs, [](u32 word) { return word & ~single::kSignMask; });
}
// The scaled conversions read and write fixed point
constexpr u32 encodePowerOfTwo(s32 exponent) { return static_cast<u32>(127 + exponent) << 23; }

inline void vitof(Vu& vu0, u32 dest, u32 vt, u32 vs, s32 fraction) {
    vectorUnary(vu0, dest, vt, vs, [fraction](u32 word) {
        const u32 value = single::convertFromInt32(static_cast<s32>(word));
        if (fraction == 0) {
            return value;
        }
        return single::clampToVu(single::mul(value, encodePowerOfTwo(-fraction)).bits);
    });
}

inline void vftoi(Vu& vu0, u32 dest, u32 vt, u32 vs, s32 fraction) {
    vectorUnary(vu0, dest, vt, vs, [fraction](u32 word) {
        const u32 scaled =
            fraction == 0 ? word
                          : single::clampToVu(single::mul(word, encodePowerOfTwo(fraction)).bits);
        return static_cast<u32>(single::convertToInt32(scaled));
    });
}

inline void viadd(Vu& vu0, u32 vid, u32 vis, u32 vit) {
    vu0.writeVi(vid, vu0.readVi(vis) + vu0.readVi(vit));
}
inline void visub(Vu& vu0, u32 vid, u32 vis, u32 vit) {
    vu0.writeVi(vid, vu0.readVi(vis) - vu0.readVi(vit));
}
inline void viand(Vu& vu0, u32 vid, u32 vis, u32 vit) {
    vu0.writeVi(vid, vu0.readVi(vis) & vu0.readVi(vit));
}
inline void vior(Vu& vu0, u32 vid, u32 vis, u32 vit) {
    vu0.writeVi(vid, vu0.readVi(vis) | vu0.readVi(vit));
}
inline void viaddi(Vu& vu0, u32 vit, u32 vis, s32 imm) {
    vu0.writeVi(vit, vu0.readVi(vis) + static_cast<u32>(imm));
}

inline u32 getDataAddress(u32 pointer) {
    return kVu0DataBase + (pointer * 16u) % kVu0MemorySize;
}

// Both units run these, so the window they address is the caller's to say.
inline u32 getIntegerAddress(const Vu& vu, u32 vis, u32 base, u32 size, s32 offset) {
    return base + ((vu.readVi(vis) + static_cast<u32>(offset)) * 16u) % size;
}

// The field is encoded into one lane rather than picked apart, so several bits
// can name a lane outside them and no bits still names one.
inline u32 getIntegerLane(u32 dest) {
    const bool x = (dest & 8u) != 0, y = (dest & 4u) != 0, z = (dest & 2u) != 0;
    return (x == y ? 2u : 0u) | (!x && !z ? 1u : 0u);
}

inline void vilwr(Vu& vu, Memory& memory, u32 dest, u32 vit, u32 vis, u32 base, u32 size,
                  s32 offset = 0) {
    const u32 address = getIntegerAddress(vu, vis, base, size, offset);
    vu.writeVi(vit, memory.read32(address + 4u * getIntegerLane(dest)));
}
inline void viswr(Vu& vu, Memory& memory, u32 dest, u32 vit, u32 vis, u32 base, u32 size,
                  s32 offset = 0) {
    const u32 address = getIntegerAddress(vu, vis, base, size, offset);
    for (s32 i = 0; i < 4; ++i) {
        if (Vu::writes(dest, i)) {
            memory.write32(address + 4u * static_cast<u32>(i), vu.readVi(vit));
        }
    }
}
inline void vsqi(Vu& vu0, Memory& memory, u32 dest, u32 vs, u32 vit) {
    const u32 pointer = vu0.readVi(vit);
    const Reg128 value = vu0.readVf(vs);
    const u32 address = getDataAddress(pointer);
    for (s32 i = 0; i < 4; ++i) {
        if (Vu::writes(dest, i)) {
            memory.write32(address + 4u * static_cast<u32>(i), value.word(i));
        }
    }
    vu0.writeVi(vit, pointer + 1);
}

inline bool isZeroLane(u32 bits) {
    return (single::flushDenormal(bits) & single::kMagnitudeMask) == 0;
}

// The divider truncates twice, which no single rounding reproduces.
inline Vu::Quotient computeVuQuotient(u32 a, u32 b) {
    if (isZeroLane(b)) {
        return {((a ^ b) & single::kSignMask) | single::kLargest, isZeroLane(a),
                !isZeroLane(a)};
    }
    return {clampVuResult(single::div(a, b)), false, false};
}

// The sign is dropped rather than answered, so a negative radicand is only
// reported, never refused.
inline Vu::Quotient computeVuRoot(u32 b) {
    return {clampVuResult(single::sqrt(b)), (b & single::kSignMask) != 0, false};
}

inline Vu::Quotient computeVuInverseRoot(u32 a, u32 b) {
    const bool negative = (b & single::kSignMask) != 0;
    // The root has no sign left to contribute, so the numerator carries it.
    if (isZeroLane(b)) {
        return {(a & single::kSignMask) | single::kLargest,
                negative || isZeroLane(a), !isZeroLane(a)};
    }
    return {clampVuResult(single::rsqrt(a, b)), negative, false};
}

inline void vdiv(Vu& vu0, u32 vs, u32 fsf, u32 vt, u32 ftf) {
    const Vu::Quotient q = computeVuQuotient(vu0.readVf(vs).word(static_cast<s32>(fsf)),
                              vu0.readVf(vt).word(static_cast<s32>(ftf)));
    vu0.setDivideFlags(q.invalid, q.byZero);
    vu0.vi[Vu::kQ] = q.value;
}

inline void vsqrt(Vu& vu0, u32 vt, u32 ftf) {
    const Vu::Quotient q = computeVuRoot(vu0.readVf(vt).word(static_cast<s32>(ftf)));
    vu0.setDivideFlags(q.invalid, q.byZero);
    vu0.vi[Vu::kQ] = q.value;
}

inline void vrsqrt(Vu& vu0, u32 vs, u32 fsf, u32 vt, u32 ftf) {
    const Vu::Quotient q = computeVuInverseRoot(vu0.readVf(vs).word(static_cast<s32>(fsf)),
                                   vu0.readVf(vt).word(static_cast<s32>(ftf)));
    vu0.setDivideFlags(q.invalid, q.byZero);
    vu0.vi[Vu::kQ] = q.value;
}

// The outer product is defined on xyz alone, whatever the encoding says.
inline void vclip(Vu& vu0, u32 vs, u32 vt) {
    const Reg128 s = vu0.readVf(vs);
    const f64 limit = std::fabs(single::widen(vu0.readVf(vt).word(3)));
    u32 bits = 0;
    for (s32 i = 0; i < 3; ++i) {
        const f64 value = single::widen(s.word(i));
        if (value > limit) {
            bits |= 1u << (2 * i);
        }
        if (value < -limit) {
            bits |= 1u << (2 * i + 1);
        }
    }
    vu0.clip = ((vu0.clip << 6) | bits) & 0xFFFFFFu;
}

constexpr u32 kOuterProductDest = 0xEu;

inline void vopmula(Vu& vu0, u32 dest, u32 vs, u32 vt) {
    const Reg128 a = vu0.readVf(vs);
    const Reg128 b = vu0.readVf(vt);
    u32 out[4]{};
    u32 flags[4]{};
    bool saturated[4]{};
    for (s32 i = 0; i < 3; ++i) {
        const single::Rounded rounded = single::mul(a.word((i + 1) % 3), b.word((i + 2) % 3));
        out[i] = clampVuResult(rounded);
        flags[i] = getRangeFlags(rounded);
        saturated[i] = rounded.overflow;
    }
    const u32 written = dest & kOuterProductDest;
    vu0.setFlags(written, out, flags);
    vu0.writeAcc(written, out, saturated);
}
inline void vopmsub(Vu& vu0, u32 dest, u32 vd, u32 vs, u32 vt) {
    const Reg128 a = vu0.readVf(vs);
    const Reg128 b = vu0.readVf(vt);
    const u32 written = dest & kOuterProductDest;
    u32 product[4]{};
    u32 productFlags[4]{};
    u32 out[4]{};
    u32 flags[4]{};
    for (s32 i = 0; i < 3; ++i) {
        const single::Rounded multiplied =
            single::mul(a.word((i + 1) % 3), b.word((i + 2) % 3));
        product[i] = clampVuResult(multiplied);
        productFlags[i] = getRangeFlags(multiplied);

        const single::Rounded rounded = single::multiplyAdd(
            vu0.acc.word(i), a.word((i + 1) % 3), b.word((i + 2) % 3), true);
        out[i] = clampVuResult(rounded);
        flags[i] = getRangeFlags(rounded);
        // A product the multiplier could not hold saturates before the adder
        // sees it, so the difference saturates with it.
        if (vu0.accSaturated[i] || (productFlags[i] & Vu::kOverflow) != 0) {
            flags[i] |= Vu::kOverflow;
            out[i] = (out[i] & single::kSignMask) | single::kLargest;
        }
    }
    vu0.addSticky(written, product, productFlags);
    vu0.setFlags(written, out, flags);
    vu0.writeVf(vd, written, out);
}

// The microprogram runs to completion here, so a guest that polls for the unit
// to finish never sees it busy. TPC keeps where it stopped.
inline void vcallms(Vu& vu0, Memory& memory, u32 program) {
    vu0.keepBeforeCall();
    runMicroProgram(vu0, memory, nullptr, 0, program);
    vu0.started = true;
    vu0.syncFlags();
}
inline void vcallmsr(Vu& vu0, Memory& memory) {
    vcallms(vu0, memory, vu0.vi[Vu::kCmsar0]);
}

inline bool findVuForm(Op op, VuForm& form) {
    switch (op) {
        case Op::vadd: form = {VuKind::Add, VuSource::Vt, false}; return true;
        case Op::vaddx: form = {VuKind::Add, VuSource::X, false}; return true;
        case Op::vaddy: form = {VuKind::Add, VuSource::Y, false}; return true;
        case Op::vaddz: form = {VuKind::Add, VuSource::Z, false}; return true;
        case Op::vaddw: form = {VuKind::Add, VuSource::W, false}; return true;
        case Op::vaddq: form = {VuKind::Add, VuSource::Q, false}; return true;
        case Op::vaddi: form = {VuKind::Add, VuSource::I, false}; return true;
        case Op::vsub: form = {VuKind::Sub, VuSource::Vt, false}; return true;
        case Op::vsubx: form = {VuKind::Sub, VuSource::X, false}; return true;
        case Op::vsuby: form = {VuKind::Sub, VuSource::Y, false}; return true;
        case Op::vsubz: form = {VuKind::Sub, VuSource::Z, false}; return true;
        case Op::vsubw: form = {VuKind::Sub, VuSource::W, false}; return true;
        case Op::vsubq: form = {VuKind::Sub, VuSource::Q, false}; return true;
        case Op::vsubi: form = {VuKind::Sub, VuSource::I, false}; return true;
        case Op::vmul: form = {VuKind::Mul, VuSource::Vt, false}; return true;
        case Op::vmulx: form = {VuKind::Mul, VuSource::X, false}; return true;
        case Op::vmuly: form = {VuKind::Mul, VuSource::Y, false}; return true;
        case Op::vmulz: form = {VuKind::Mul, VuSource::Z, false}; return true;
        case Op::vmulw: form = {VuKind::Mul, VuSource::W, false}; return true;
        case Op::vmulq: form = {VuKind::Mul, VuSource::Q, false}; return true;
        case Op::vmuli: form = {VuKind::Mul, VuSource::I, false}; return true;
        case Op::vmax: form = {VuKind::Max, VuSource::Vt, false}; return true;
        case Op::vmaxx: form = {VuKind::Max, VuSource::X, false}; return true;
        case Op::vmaxy: form = {VuKind::Max, VuSource::Y, false}; return true;
        case Op::vmaxz: form = {VuKind::Max, VuSource::Z, false}; return true;
        case Op::vmaxw: form = {VuKind::Max, VuSource::W, false}; return true;
        case Op::vmaxi: form = {VuKind::Max, VuSource::I, false}; return true;
        case Op::vmini: form = {VuKind::Mini, VuSource::Vt, false}; return true;
        case Op::vminix: form = {VuKind::Mini, VuSource::X, false}; return true;
        case Op::vminiy: form = {VuKind::Mini, VuSource::Y, false}; return true;
        case Op::vminiz: form = {VuKind::Mini, VuSource::Z, false}; return true;
        case Op::vminiw: form = {VuKind::Mini, VuSource::W, false}; return true;
        case Op::vminii: form = {VuKind::Mini, VuSource::I, false}; return true;
        case Op::vmadd: form = {VuKind::Madd, VuSource::Vt, false}; return true;
        case Op::vmaddx: form = {VuKind::Madd, VuSource::X, false}; return true;
        case Op::vmaddy: form = {VuKind::Madd, VuSource::Y, false}; return true;
        case Op::vmaddz: form = {VuKind::Madd, VuSource::Z, false}; return true;
        case Op::vmaddw: form = {VuKind::Madd, VuSource::W, false}; return true;
        case Op::vmaddq: form = {VuKind::Madd, VuSource::Q, false}; return true;
        case Op::vmaddi: form = {VuKind::Madd, VuSource::I, false}; return true;
        case Op::vmsub: form = {VuKind::Msub, VuSource::Vt, false}; return true;
        case Op::vmsubx: form = {VuKind::Msub, VuSource::X, false}; return true;
        case Op::vmsuby: form = {VuKind::Msub, VuSource::Y, false}; return true;
        case Op::vmsubz: form = {VuKind::Msub, VuSource::Z, false}; return true;
        case Op::vmsubw: form = {VuKind::Msub, VuSource::W, false}; return true;
        case Op::vmsubq: form = {VuKind::Msub, VuSource::Q, false}; return true;
        case Op::vmsubi: form = {VuKind::Msub, VuSource::I, false}; return true;
        case Op::vadda: form = {VuKind::Add, VuSource::Vt, true}; return true;
        case Op::vaddax: form = {VuKind::Add, VuSource::X, true}; return true;
        case Op::vadday: form = {VuKind::Add, VuSource::Y, true}; return true;
        case Op::vaddaz: form = {VuKind::Add, VuSource::Z, true}; return true;
        case Op::vaddaw: form = {VuKind::Add, VuSource::W, true}; return true;
        case Op::vaddaq: form = {VuKind::Add, VuSource::Q, true}; return true;
        case Op::vaddai: form = {VuKind::Add, VuSource::I, true}; return true;
        case Op::vsuba: form = {VuKind::Sub, VuSource::Vt, true}; return true;
        case Op::vsubax: form = {VuKind::Sub, VuSource::X, true}; return true;
        case Op::vsubay: form = {VuKind::Sub, VuSource::Y, true}; return true;
        case Op::vsubaz: form = {VuKind::Sub, VuSource::Z, true}; return true;
        case Op::vsubaw: form = {VuKind::Sub, VuSource::W, true}; return true;
        case Op::vsubaq: form = {VuKind::Sub, VuSource::Q, true}; return true;
        case Op::vsubai: form = {VuKind::Sub, VuSource::I, true}; return true;
        case Op::vmula: form = {VuKind::Mul, VuSource::Vt, true}; return true;
        case Op::vmulax: form = {VuKind::Mul, VuSource::X, true}; return true;
        case Op::vmulay: form = {VuKind::Mul, VuSource::Y, true}; return true;
        case Op::vmulaz: form = {VuKind::Mul, VuSource::Z, true}; return true;
        case Op::vmulaw: form = {VuKind::Mul, VuSource::W, true}; return true;
        case Op::vmulaq: form = {VuKind::Mul, VuSource::Q, true}; return true;
        case Op::vmulai: form = {VuKind::Mul, VuSource::I, true}; return true;
        case Op::vmadda: form = {VuKind::Madd, VuSource::Vt, true}; return true;
        case Op::vmaddax: form = {VuKind::Madd, VuSource::X, true}; return true;
        case Op::vmadday: form = {VuKind::Madd, VuSource::Y, true}; return true;
        case Op::vmaddaz: form = {VuKind::Madd, VuSource::Z, true}; return true;
        case Op::vmaddaw: form = {VuKind::Madd, VuSource::W, true}; return true;
        case Op::vmaddaq: form = {VuKind::Madd, VuSource::Q, true}; return true;
        case Op::vmaddai: form = {VuKind::Madd, VuSource::I, true}; return true;
        case Op::vmsuba: form = {VuKind::Msub, VuSource::Vt, true}; return true;
        case Op::vmsubax: form = {VuKind::Msub, VuSource::X, true}; return true;
        case Op::vmsubay: form = {VuKind::Msub, VuSource::Y, true}; return true;
        case Op::vmsubaz: form = {VuKind::Msub, VuSource::Z, true}; return true;
        case Op::vmsubaw: form = {VuKind::Msub, VuSource::W, true}; return true;
        case Op::vmsubaq: form = {VuKind::Msub, VuSource::Q, true}; return true;
        case Op::vmsubai: form = {VuKind::Msub, VuSource::I, true}; return true;
        default: return false;
    }
}

}  // namespace uee
