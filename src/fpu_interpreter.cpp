#include "fpu_interpreter.h"

#include "instruction.h"
#include "single.h"

#include <cmath>

namespace uee {

bool FpuInterpreter::execute(Op op, Instruction insn) {
    switch (op) {
    case Op::add_s:
    case Op::sub_s:
    case Op::mul_s:
    case Op::div_s: {
        const f64 a = single::toFloat(mState.fpr[insn.fs()]);
        const f64 b = single::toFloat(mState.fpr[insn.ft()]);
        mState.fpr[insn.fd()] = (op == Op::add_s)   ? single::truncate(single::addExact(a, b))
                         : (op == Op::sub_s) ? single::truncate(single::addExact(a, -b))
                         : (op == Op::mul_s) ? single::truncate(a * b)
                                             : single::truncate(a / b);
        break;
    }
    case Op::sqrt_s:
        mState.fpr[insn.fd()] = single::truncate(std::sqrt(f64{single::toFloat(mState.fpr[insn.ft()])}));
        break;
    case Op::abs_s: mState.fpr[insn.fd()] = mState.fpr[insn.fs()] & ~single::kSignMask; break;
    case Op::neg_s: mState.fpr[insn.fd()] = mState.fpr[insn.fs()] ^ single::kSignMask; break;
    case Op::mov_s: mState.fpr[insn.fd()] = mState.fpr[insn.fs()]; break;
    case Op::cvt_w_s:
        mState.fpr[insn.fd()] = static_cast<u32>(static_cast<s32>(single::toFloat(mState.fpr[insn.fs()])));
        break;
    case Op::cvt_s_w:
        mState.fpr[insn.fd()] = single::toBits(static_cast<f32>(static_cast<s32>(mState.fpr[insn.fs()])));
        break;
    case Op::c_eq_s:
    case Op::c_lt_s:
    case Op::c_le_s: {
        const f32 a = single::toFloat(mState.fpr[insn.fs()]);
        const f32 b = single::toFloat(mState.fpr[insn.ft()]);
        const bool set = (op == Op::c_eq_s)   ? (a == b)
                         : (op == Op::c_lt_s) ? (a < b)
                                              : (a <= b);
        mState.fcr31 = set ? (mState.fcr31 | Fpu::kCompareBit) : (mState.fcr31 & ~Fpu::kCompareBit);
        break;
    }

    case Op::adda_s:
    case Op::suba_s:
    case Op::mula_s: {
        const f64 a = single::toFloat(mState.fpr[insn.fs()]);
        const f64 b = single::toFloat(mState.fpr[insn.ft()]);
        mState.acc = (op == Op::adda_s)   ? single::truncate(single::addExact(a, b))
              : (op == Op::suba_s) ? single::truncate(single::addExact(a, -b))
                                   : single::truncate(a * b);
        break;
    }
    case Op::madda_s:
    case Op::msuba_s: {
        const f64 product = f64{single::toFloat(mState.fpr[insn.fs()])} * single::toFloat(mState.fpr[insn.ft()]);
        mState.acc = single::truncate(single::addExact(single::toFloat(mState.acc),
                                                (op == Op::madda_s) ? product : -product));
        break;
    }
    case Op::madd_s:
    case Op::msub_s: {
        // the product rounds to single before it reaches the accumulator, which
        // is what separates these from madda.s and msuba.s
        const f64 product = single::toFloat(single::truncate(
            f64{single::toFloat(mState.fpr[insn.fs()])} * single::toFloat(mState.fpr[insn.ft()])));
        mState.fpr[insn.fd()] = single::truncate(single::addExact(
            single::toFloat(mState.acc), (op == Op::madd_s) ? product : -product));
        break;
    }

    default:
        return false;
    }
    return true;
}

}  // namespace uee
