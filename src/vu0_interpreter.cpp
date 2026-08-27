#include "vu0_interpreter.h"

#include "instruction.h"
#include "single.h"

#include <cmath>

namespace uee {
namespace {

enum class Kind { Add, Sub, Mul, Max, Mini, Madd, Msub };
enum class Source { Vt, X, Y, Z, W, Q, I };

struct Form {
    Kind kind;
    Source source;
    bool toAcc;
};

f64 lane(const Reg128& r, s32 i) {
    return single::widen(r.word(i));
}

f64 squareRoot(const Reg128& vt, u32 field) {
    return std::sqrt(std::abs(lane(vt, static_cast<s32>(field))));
}

u32 round(single::Wide exact) {
    return single::clampToVu(single::truncate(exact).bits);
}

u32 round(f64 exact) {
    return round(single::Wide{exact, 0});
}

s32 signedImmediate5(Instruction insn) {
    return static_cast<s32>(insn.sa() << 27) >> 27;
}

bool formOf(Op op, Form& form) {
#define R5900_VU_FORM(name, kind, source, toAcc) \
    case Op::name:                               \
        form = {Kind::kind, Source::source, toAcc}; \
        return true;

    switch (op) {
        R5900_VU_FORM(vaddi, Add, I, false)
        R5900_VU_FORM(vsubi, Sub, I, false)
        R5900_VU_FORM(vmuli, Mul, I, false)
        R5900_VU_FORM(vmaddi, Madd, I, false)
        R5900_VU_FORM(vmsubi, Msub, I, false)
        R5900_VU_FORM(vmaxi, Max, I, false)
        R5900_VU_FORM(vminii, Mini, I, false)
        R5900_VU_FORM(vaddai, Add, I, true)
        R5900_VU_FORM(vsubai, Sub, I, true)
        R5900_VU_FORM(vmulai, Mul, I, true)
        R5900_VU_FORM(vmaddai, Madd, I, true)
        R5900_VU_FORM(vmsubai, Msub, I, true)
        R5900_VU_FORM(vadd, Add, Vt, false)
        R5900_VU_FORM(vaddx, Add, X, false)
        R5900_VU_FORM(vaddy, Add, Y, false)
        R5900_VU_FORM(vaddz, Add, Z, false)
        R5900_VU_FORM(vaddw, Add, W, false)
        R5900_VU_FORM(vaddq, Add, Q, false)
        R5900_VU_FORM(vsub, Sub, Vt, false)
        R5900_VU_FORM(vsubx, Sub, X, false)
        R5900_VU_FORM(vsuby, Sub, Y, false)
        R5900_VU_FORM(vsubz, Sub, Z, false)
        R5900_VU_FORM(vsubw, Sub, W, false)
        R5900_VU_FORM(vsubq, Sub, Q, false)
        R5900_VU_FORM(vmul, Mul, Vt, false)
        R5900_VU_FORM(vmulx, Mul, X, false)
        R5900_VU_FORM(vmuly, Mul, Y, false)
        R5900_VU_FORM(vmulz, Mul, Z, false)
        R5900_VU_FORM(vmulw, Mul, W, false)
        R5900_VU_FORM(vmulq, Mul, Q, false)
        R5900_VU_FORM(vmax, Max, Vt, false)
        R5900_VU_FORM(vmaxx, Max, X, false)
        R5900_VU_FORM(vmaxy, Max, Y, false)
        R5900_VU_FORM(vmaxz, Max, Z, false)
        R5900_VU_FORM(vmaxw, Max, W, false)
        R5900_VU_FORM(vmini, Mini, Vt, false)
        R5900_VU_FORM(vminix, Mini, X, false)
        R5900_VU_FORM(vminiy, Mini, Y, false)
        R5900_VU_FORM(vminiz, Mini, Z, false)
        R5900_VU_FORM(vminiw, Mini, W, false)
        R5900_VU_FORM(vmadd, Madd, Vt, false)
        R5900_VU_FORM(vmaddx, Madd, X, false)
        R5900_VU_FORM(vmaddy, Madd, Y, false)
        R5900_VU_FORM(vmaddz, Madd, Z, false)
        R5900_VU_FORM(vmaddw, Madd, W, false)
        R5900_VU_FORM(vmaddq, Madd, Q, false)
        R5900_VU_FORM(vmsub, Msub, Vt, false)
        R5900_VU_FORM(vmsubx, Msub, X, false)
        R5900_VU_FORM(vmsuby, Msub, Y, false)
        R5900_VU_FORM(vmsubz, Msub, Z, false)
        R5900_VU_FORM(vmsubw, Msub, W, false)
        R5900_VU_FORM(vmsubq, Msub, Q, false)
        R5900_VU_FORM(vadda, Add, Vt, true)
        R5900_VU_FORM(vaddax, Add, X, true)
        R5900_VU_FORM(vadday, Add, Y, true)
        R5900_VU_FORM(vaddaz, Add, Z, true)
        R5900_VU_FORM(vaddaw, Add, W, true)
        R5900_VU_FORM(vaddaq, Add, Q, true)
        R5900_VU_FORM(vsuba, Sub, Vt, true)
        R5900_VU_FORM(vsubax, Sub, X, true)
        R5900_VU_FORM(vsubay, Sub, Y, true)
        R5900_VU_FORM(vsubaz, Sub, Z, true)
        R5900_VU_FORM(vsubaw, Sub, W, true)
        R5900_VU_FORM(vsubaq, Sub, Q, true)
        R5900_VU_FORM(vmula, Mul, Vt, true)
        R5900_VU_FORM(vmulax, Mul, X, true)
        R5900_VU_FORM(vmulay, Mul, Y, true)
        R5900_VU_FORM(vmulaz, Mul, Z, true)
        R5900_VU_FORM(vmulaw, Mul, W, true)
        R5900_VU_FORM(vmulaq, Mul, Q, true)
        R5900_VU_FORM(vmadda, Madd, Vt, true)
        R5900_VU_FORM(vmaddax, Madd, X, true)
        R5900_VU_FORM(vmadday, Madd, Y, true)
        R5900_VU_FORM(vmaddaz, Madd, Z, true)
        R5900_VU_FORM(vmaddaw, Madd, W, true)
        R5900_VU_FORM(vmaddaq, Madd, Q, true)
        R5900_VU_FORM(vmsuba, Msub, Vt, true)
        R5900_VU_FORM(vmsubax, Msub, X, true)
        R5900_VU_FORM(vmsubay, Msub, Y, true)
        R5900_VU_FORM(vmsubaz, Msub, Z, true)
        R5900_VU_FORM(vmsubaw, Msub, W, true)
        R5900_VU_FORM(vmsubaq, Msub, Q, true)
        default:
            return false;
    }
#undef R5900_VU_FORM
}

}  // namespace

bool Vu0Interpreter::execute(Op op, Instruction insn) {
    switch (op) {
    case Op::vmove:
    case Op::vabs:
    case Op::vitof0:
    case Op::vftoi0: {
        const Reg128 vs = mState.readVf(insn.vs());
        u32 out[4];
        for (s32 i = 0; i < 4; ++i) {
            switch (op) {
                case Op::vabs: out[i] = vs.word(i) & ~single::kSignMask; break;
                case Op::vitof0:
                    out[i] = single::fromInt32(static_cast<s32>(vs.word(i)));
                    break;
                case Op::vftoi0:
                    out[i] = static_cast<u32>(single::toInt32(vs.word(i)));
                    break;
                default: out[i] = vs.word(i); break;
            }
        }
        mState.writeVf(insn.vt(), insn.dest(), out);
        break;
    }

    // The Q register is written the moment the divider is issued, so nothing
    // waits and nothing stalls.
    case Op::vnop:
    case Op::vwaitq:
        break;

    case Op::viadd:
        mState.writeVi(insn.vid(), mState.readVi(insn.vis()) + mState.readVi(insn.vit()));
        break;
    case Op::visub:
        mState.writeVi(insn.vid(), mState.readVi(insn.vis()) - mState.readVi(insn.vit()));
        break;
    case Op::viand:
        mState.writeVi(insn.vid(), mState.readVi(insn.vis()) & mState.readVi(insn.vit()));
        break;
    case Op::vior:
        mState.writeVi(insn.vid(), mState.readVi(insn.vis()) | mState.readVi(insn.vit()));
        break;
    case Op::viaddi:
        mState.writeVi(insn.vit(),
                       mState.readVi(insn.vis()) + static_cast<u32>(signedImmediate5(insn)));
        break;

    // The integer loads and stores name one lane of a quadword in VU0 data
    // memory, addressed by quadword index.
    case Op::vilwr:
    case Op::viswr: {
        const u32 address =
            kVu0DataBase + (mState.readVi(insn.vis()) * 16u) % kVu0MemorySize;
        for (s32 i = 0; i < 4; ++i) {
            if (!Vu0::writes(insn.dest(), i)) {
                continue;
            }
            const u32 lane = address + 4u * static_cast<u32>(i);
            if (op == Op::vilwr) {
                mState.writeVi(insn.vit(), mMemory.read32(lane));
            } else {
                mMemory.write32(lane, mState.readVi(insn.vit()));
            }
        }
        break;
    }

    case Op::vsqi: {
        const u32 pointer = mState.readVi(insn.vit());
        const Reg128 value = mState.readVf(insn.vs());
        const u32 address = kVu0DataBase + (pointer * 16u) % kVu0MemorySize;
        for (s32 i = 0; i < 4; ++i) {
            if (Vu0::writes(insn.dest(), i)) {
                mMemory.write32(address + 4u * static_cast<u32>(i), value.word(i));
            }
        }
        mState.writeVi(insn.vit(), pointer + 1);
        break;
    }

    // the divider truncates twice, once taking the root and once taking the
    // quotient, and no single rounding reproduces it
    case Op::vsqrt:
        mState.vi[Vu0::kQ] = round(squareRoot(mState.readVf(insn.vt()), insn.ftf()));
        break;
    case Op::vrsqrt: {
        const f64 root = single::widen(round(squareRoot(mState.readVf(insn.vt()), insn.ftf())));
        mState.vi[Vu0::kQ] =
            round(lane(mState.readVf(insn.vs()), static_cast<s32>(insn.fsf())) / root);
        break;
    }

    case Op::vopmula:
    case Op::vopmsub: {
        const Reg128 vs = mState.readVf(insn.vs());
        const Reg128 vt = mState.readVf(insn.vt());
        u32 out[4]{};
        for (s32 i = 0; i < 3; ++i) {
            const f64 product = lane(vs, (i + 1) % 3) * lane(vt, (i + 2) % 3);
            out[i] = (op == Op::vopmula)
                         ? round(product)
                         : round(single::addExact(lane(mState.acc, i), -product));
        }
        // the outer product is defined on xyz alone, whatever the encoding says
        const u32 dest = insn.dest() & 0xEu;
        if (op == Op::vopmula) {
            mState.writeAcc(dest, out);
        } else {
            mState.writeVf(insn.vd(), dest, out);
        }
        break;
    }

    default: {
        Form form;
        if (!formOf(op, form)) {
            return false;
        }
        const Reg128 vs = mState.readVf(insn.vs());
        const Reg128 vt = mState.readVf(insn.vt());
        u32 out[4];
        for (s32 i = 0; i < 4; ++i) {
            const f64 a = lane(vs, i);
            f64 b;
            switch (form.source) {
                case Source::X: b = lane(vt, 0); break;
                case Source::Y: b = lane(vt, 1); break;
                case Source::Z: b = lane(vt, 2); break;
                case Source::W: b = lane(vt, 3); break;
                case Source::Q: b = single::widen(mState.vi[Vu0::kQ]); break;
                case Source::I: b = single::widen(mState.vi[Vu0::kI]); break;
                default: b = lane(vt, i); break;
            }
            single::Wide value;
            switch (form.kind) {
                case Kind::Add: value = single::addExact(a, b); break;
                case Kind::Sub: value = single::addExact(a, -b); break;
                case Kind::Mul: value = {a * b, 0}; break;
                case Kind::Max: value = {(a > b) ? a : b, 0}; break;
                case Kind::Mini: value = {(a < b) ? a : b, 0}; break;
                case Kind::Madd:
                case Kind::Msub: {
                    const f64 product = a * b;
                    value = single::addExact(lane(mState.acc, i),
                                          (form.kind == Kind::Madd) ? product : -product);
                    break;
                }
            }
            out[i] = round(value);
        }
        if (form.toAcc) {
            mState.writeAcc(insn.dest(), out);
        } else {
            mState.writeVf(insn.vd(), insn.dest(), out);
        }
        break;
    }
    }
    return true;
}

}  // namespace uee
