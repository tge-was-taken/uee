#pragma once

#include "memory.h"
#include "opcodes.h"
#include "single.h"
#include "types.h"
#include "vu.h"
#include "vu_micro.h"

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

inline f64 vuLane(const Reg128& r, s32 i) {
    return single::widen(r.word(i));
}

inline u32 vuRound(single::Wide exact) {
    return single::clampToVu(single::truncate(exact).bits);
}

inline u32 vuRound(f64 exact) {
    return vuRound(single::Wide{exact, 0});
}

inline u32 vuOperand(const Vu& vu0, VuSource source, const Reg128& vt, s32 i) {
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

// The adder loses whatever shifts past its width, so an operand small enough
// against the other leaves it unchanged.
inline u32 vuCombine(const Vu& vu0, VuKind kind, u32 a, u32 b, s32 i) {
    const f64 x = single::widen(a), y = single::widen(b);
    switch (kind) {
        case VuKind::Add: return single::clampToVu(single::add(a, b).bits);
        case VuKind::Sub: return single::clampToVu(single::sub(a, b).bits);
        case VuKind::Mul: return single::clampToVu(single::mul(a, b).bits);
        case VuKind::Max: return vuRound((x > y) ? x : y);
        case VuKind::Mini: return vuRound((x < y) ? x : y);
        // The accumulate keeps full precision here where the adder above does
        // not: no hardware case pins down which the accumulator does.
        case VuKind::Madd: return vuRound(single::addExact(vuLane(vu0.acc, i), x * y));
        case VuKind::Msub: return vuRound(single::addExact(vuLane(vu0.acc, i), -(x * y)));
    }
    return 0;
}

inline void vectorOp(Vu& vu0, VuForm form, u32 dest, u32 vd, u32 vs, u32 vt) {
    const Reg128 a = vu0.readVf(vs);
    const Reg128 b = vu0.readVf(vt);
    u32 out[4];
    for (s32 i = 0; i < 4; ++i) {
        out[i] = vuCombine(vu0, form.kind, a.word(i), vuOperand(vu0, form.source, b, i), i);
    }
    if (form.toAcc) {
        vu0.writeAcc(dest, out);
    } else {
        vu0.writeVf(vd, dest, out);
    }
}

inline void vadd(Vu& vu0, VuSource from, u32 dest, u32 vd, u32 vs, u32 vt) {
    vectorOp(vu0, {VuKind::Add, from, false}, dest, vd, vs, vt);
}
inline void vsub(Vu& vu0, VuSource from, u32 dest, u32 vd, u32 vs, u32 vt) {
    vectorOp(vu0, {VuKind::Sub, from, false}, dest, vd, vs, vt);
}
inline void vmul(Vu& vu0, VuSource from, u32 dest, u32 vd, u32 vs, u32 vt) {
    vectorOp(vu0, {VuKind::Mul, from, false}, dest, vd, vs, vt);
}
inline void vmax(Vu& vu0, VuSource from, u32 dest, u32 vd, u32 vs, u32 vt) {
    vectorOp(vu0, {VuKind::Max, from, false}, dest, vd, vs, vt);
}
inline void vmini(Vu& vu0, VuSource from, u32 dest, u32 vd, u32 vs, u32 vt) {
    vectorOp(vu0, {VuKind::Mini, from, false}, dest, vd, vs, vt);
}
inline void vmadd(Vu& vu0, VuSource from, u32 dest, u32 vd, u32 vs, u32 vt) {
    vectorOp(vu0, {VuKind::Madd, from, false}, dest, vd, vs, vt);
}
inline void vmsub(Vu& vu0, VuSource from, u32 dest, u32 vd, u32 vs, u32 vt) {
    vectorOp(vu0, {VuKind::Msub, from, false}, dest, vd, vs, vt);
}

inline void vadda(Vu& vu0, VuSource from, u32 dest, u32 vs, u32 vt) {
    vectorOp(vu0, {VuKind::Add, from, true}, dest, 0, vs, vt);
}
inline void vsuba(Vu& vu0, VuSource from, u32 dest, u32 vs, u32 vt) {
    vectorOp(vu0, {VuKind::Sub, from, true}, dest, 0, vs, vt);
}
inline void vmula(Vu& vu0, VuSource from, u32 dest, u32 vs, u32 vt) {
    vectorOp(vu0, {VuKind::Mul, from, true}, dest, 0, vs, vt);
}
inline void vmadda(Vu& vu0, VuSource from, u32 dest, u32 vs, u32 vt) {
    vectorOp(vu0, {VuKind::Madd, from, true}, dest, 0, vs, vt);
}
inline void vmsuba(Vu& vu0, VuSource from, u32 dest, u32 vs, u32 vt) {
    vectorOp(vu0, {VuKind::Msub, from, true}, dest, 0, vs, vt);
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

inline void vmfir(Vu& vu0, u32 dest, u32 vt, u32 vis) {
    vbroadcast(vu0, dest, vt, vu0.readVi(vis));
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
constexpr u32 powerOfTwo(s32 exponent) { return static_cast<u32>(127 + exponent) << 23; }

inline void vitof(Vu& vu0, u32 dest, u32 vt, u32 vs, s32 fraction) {
    vectorUnary(vu0, dest, vt, vs, [fraction](u32 word) {
        const u32 value = single::fromInt32(static_cast<s32>(word));
        if (fraction == 0) {
            return value;
        }
        return single::clampToVu(single::mul(value, powerOfTwo(-fraction)).bits);
    });
}

inline void vftoi(Vu& vu0, u32 dest, u32 vt, u32 vs, s32 fraction) {
    vectorUnary(vu0, dest, vt, vs, [fraction](u32 word) {
        const u32 scaled =
            fraction == 0 ? word
                          : single::clampToVu(single::mul(word, powerOfTwo(fraction)).bits);
        return static_cast<u32>(single::toInt32(scaled));
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

inline u32 vuDataAddress(u32 pointer) {
    return kVu0DataBase + (pointer * 16u) % kVu0MemorySize;
}

inline void vilwr(Vu& vu0, Memory& memory, u32 dest, u32 vit, u32 vis) {
    const u32 address = vuDataAddress(vu0.readVi(vis));
    for (s32 i = 0; i < 4; ++i) {
        if (Vu::writes(dest, i)) {
            vu0.writeVi(vit, memory.read32(address + 4u * static_cast<u32>(i)));
        }
    }
}
inline void viswr(Vu& vu0, Memory& memory, u32 dest, u32 vit, u32 vis) {
    const u32 address = vuDataAddress(vu0.readVi(vis));
    for (s32 i = 0; i < 4; ++i) {
        if (Vu::writes(dest, i)) {
            memory.write32(address + 4u * static_cast<u32>(i), vu0.readVi(vit));
        }
    }
}
inline void vsqi(Vu& vu0, Memory& memory, u32 dest, u32 vs, u32 vit) {
    const u32 pointer = vu0.readVi(vit);
    const Reg128 value = vu0.readVf(vs);
    const u32 address = vuDataAddress(pointer);
    for (s32 i = 0; i < 4; ++i) {
        if (Vu::writes(dest, i)) {
            memory.write32(address + 4u * static_cast<u32>(i), value.word(i));
        }
    }
    vu0.writeVi(vit, pointer + 1);
}

inline f64 vuRootOfLane(const Reg128& vt, u32 field) {
    return std::sqrt(std::abs(vuLane(vt, static_cast<s32>(field))));
}

// The divider truncates twice, once taking the root and once taking the
// quotient, and no single rounding reproduces it. Q is written the moment the
// divider is issued, so nothing waits and nothing stalls.
inline void vdiv(Vu& vu0, u32 vs, u32 fsf, u32 vt, u32 ftf) {
    vu0.vi[Vu::kQ] = vuRound(vuLane(vu0.readVf(vs), static_cast<s32>(fsf)) /
                              vuLane(vu0.readVf(vt), static_cast<s32>(ftf)));
}

inline void vsqrt(Vu& vu0, u32 vt, u32 ftf) {
    vu0.vi[Vu::kQ] = vuRound(vuRootOfLane(vu0.readVf(vt), ftf));
}
inline void vrsqrt(Vu& vu0, u32 vs, u32 fsf, u32 vt, u32 ftf) {
    const f64 root = single::widen(vuRound(vuRootOfLane(vu0.readVf(vt), ftf)));
    vu0.vi[Vu::kQ] = vuRound(vuLane(vu0.readVf(vs), static_cast<s32>(fsf)) / root);
}

// The outer product is defined on xyz alone, whatever the encoding says.
// Each clip shifts six more bits in: one per axis for passing above the
// magnitude of w, and one for passing below it.
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
    for (s32 i = 0; i < 3; ++i) {
        out[i] = vuRound(vuLane(a, (i + 1) % 3) * vuLane(b, (i + 2) % 3));
    }
    vu0.writeAcc(dest & kOuterProductDest, out);
}
inline void vopmsub(Vu& vu0, u32 dest, u32 vd, u32 vs, u32 vt) {
    const Reg128 a = vu0.readVf(vs);
    const Reg128 b = vu0.readVf(vt);
    u32 out[4]{};
    for (s32 i = 0; i < 3; ++i) {
        const f64 product = vuLane(a, (i + 1) % 3) * vuLane(b, (i + 2) % 3);
        out[i] = vuRound(single::addExact(vuLane(vu0.acc, i), -product));
    }
    vu0.writeVf(vd, dest & kOuterProductDest, out);
}

// The microprogram runs to completion here, so a guest that polls for the unit
// to finish never sees it busy. TPC keeps where it stopped.
inline void vcallms(Vu& vu0, Memory& memory, u32 program) {
    vu0.vi[Vu::kTpc] = VuMicro(vu0, memory, 0).run(program);
}
inline void vcallmsr(Vu& vu0, Memory& memory) {
    vcallms(vu0, memory, vu0.vi[Vu::kCmsar0]);
}

inline bool vuFormOf(Op op, VuForm& form) {
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
