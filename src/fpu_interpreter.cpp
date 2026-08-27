#include "fpu_interpreter.h"

#include "instruction.h"
#include "single.h"

namespace uee {
namespace {

u32 causeOf(single::Rounded result) {
    return (result.overflow ? Fpu::kOverflow : 0u) | (result.underflow ? Fpu::kUnderflow : 0u);
}

}  // namespace

u32 FpuInterpreter::store(u32 result, u32 cause) {
    mState.raise(cause);
    return result;
}

bool FpuInterpreter::execute(Op op, Instruction insn) {
    const u32 fs = mState.fpr[insn.fs()];
    const u32 ft = mState.fpr[insn.ft()];

    switch (op) {
    case Op::add_s: mState.fpr[insn.fd()] = arithmetic(single::add(fs, ft)); break;
    case Op::sub_s: mState.fpr[insn.fd()] = arithmetic(single::sub(fs, ft)); break;
    case Op::mul_s: mState.fpr[insn.fd()] = arithmetic(single::mul(fs, ft)); break;
    case Op::div_s: mState.fpr[insn.fd()] = divide(fs, ft); break;
    case Op::adda_s: mState.acc = arithmetic(single::add(fs, ft)); break;
    case Op::suba_s: mState.acc = arithmetic(single::sub(fs, ft)); break;
    case Op::mula_s: mState.acc = arithmetic(single::mul(fs, ft)); break;

    case Op::madd_s: mState.fpr[insn.fd()] = accumulate(fs, ft, false); break;
    case Op::msub_s: mState.fpr[insn.fd()] = accumulate(fs, ft, true); break;
    case Op::madda_s: mState.acc = accumulate(fs, ft, false); break;
    case Op::msuba_s: mState.acc = accumulate(fs, ft, true); break;

    case Op::sqrt_s: mState.fpr[insn.fd()] = squareRoot(ft); break;
    case Op::rsqrt_s: mState.fpr[insn.fd()] = reciprocalSquareRoot(fs, ft); break;

    case Op::abs_s: mState.fpr[insn.fd()] = fs & ~single::kSignMask; break;
    case Op::neg_s: mState.fpr[insn.fd()] = fs ^ single::kSignMask; break;
    case Op::mov_s: mState.fpr[insn.fd()] = fs; break;
    case Op::max_s:
        mState.fpr[insn.fd()] =
            single::signedOrderKey(fs) < single::signedOrderKey(ft) ? ft : fs;
        break;
    case Op::min_s:
        mState.fpr[insn.fd()] =
            single::signedOrderKey(ft) < single::signedOrderKey(fs) ? ft : fs;
        break;

    case Op::cvt_w_s:
        mState.fpr[insn.fd()] = static_cast<u32>(single::toInt32(fs));
        break;
    case Op::cvt_s_w:
        mState.fpr[insn.fd()] = single::fromInt32(static_cast<s32>(fs));
        break;

    // Nothing in the format is unordered, so the four conditions the R5900
    // implements collapse to a total order.
    case Op::c_f_s: compare(false); break;
    case Op::c_eq_s: compare(single::orderKey(fs) == single::orderKey(ft)); break;
    case Op::c_lt_s: compare(single::orderKey(fs) < single::orderKey(ft)); break;
    case Op::c_le_s: compare(single::orderKey(fs) <= single::orderKey(ft)); break;

    default:
        return false;
    }
    return true;
}

u32 FpuInterpreter::arithmetic(single::Rounded result) {
    return store(result.bits, causeOf(result));
}

u32 FpuInterpreter::divide(u32 numerator, u32 denominator) {
    const bool byZero = (denominator & single::kMagnitudeMask) == 0;
    const bool zeroByZero = byZero && (numerator & single::kMagnitudeMask) == 0;
    if (byZero) {
        return store(single::div(numerator, denominator).bits,
                     zeroByZero ? Fpu::kInvalid : Fpu::kDivideByZero);
    }
    return arithmetic(single::div(numerator, denominator));
}

u32 FpuInterpreter::accumulate(u32 a, u32 b, bool subtract) {
    return arithmetic(single::multiplyAdd(mState.acc, a, b, subtract));
}

u32 FpuInterpreter::squareRoot(u32 value) {
    const single::Rounded root = single::sqrt(value);
    return store(root.bits, (value & single::kSignMask) ? Fpu::kInvalid : causeOf(root));
}

u32 FpuInterpreter::reciprocalSquareRoot(u32 numerator, u32 radicand) {
    const single::Rounded result = single::rsqrt(numerator, radicand);
    const u32 cause = (radicand & single::kSignMask)            ? Fpu::kInvalid
                      : (radicand & single::kMagnitudeMask) == 0 ? Fpu::kDivideByZero
                                                                 : causeOf(result);
    return store(result.bits, cause);
}

// A compare touches the condition bit alone. It leaves the cause bits of the
// last arithmetic operation standing rather than reporting that it raised
// nothing itself.
void FpuInterpreter::compare(bool set) {
    mState.fcr31 =
        set ? (mState.fcr31 | Fpu::kCompareBit) : (mState.fcr31 & ~Fpu::kCompareBit);
}

}  // namespace uee
