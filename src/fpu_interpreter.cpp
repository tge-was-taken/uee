#include "fpu_interpreter.h"

#include "fpu_handlers.h"
#include "instruction.h"

namespace uee {

bool executeFpu(Fpu& fpu, Op op, Instruction insn) {
    const u32 fd = insn.fd();
    const u32 fs = insn.fs();
    const u32 ft = insn.ft();

    switch (op) {
    case Op::add_s: add_s(fpu, fd, fs, ft); break;
    case Op::sub_s: sub_s(fpu, fd, fs, ft); break;
    case Op::mul_s: mul_s(fpu, fd, fs, ft); break;
    case Op::div_s: div_s(fpu, fd, fs, ft); break;
    case Op::adda_s: adda_s(fpu, fs, ft); break;
    case Op::suba_s: suba_s(fpu, fs, ft); break;
    case Op::mula_s: mula_s(fpu, fs, ft); break;

    case Op::madd_s: madd_s(fpu, fd, fs, ft); break;
    case Op::msub_s: msub_s(fpu, fd, fs, ft); break;
    case Op::madda_s: madda_s(fpu, fs, ft); break;
    case Op::msuba_s: msuba_s(fpu, fs, ft); break;

    case Op::sqrt_s: sqrt_s(fpu, fd, ft); break;
    case Op::rsqrt_s: rsqrt_s(fpu, fd, fs, ft); break;

    case Op::abs_s: abs_s(fpu, fd, fs); break;
    case Op::neg_s: neg_s(fpu, fd, fs); break;
    case Op::mov_s: mov_s(fpu, fd, fs); break;
    case Op::max_s: max_s(fpu, fd, fs, ft); break;
    case Op::min_s: min_s(fpu, fd, fs, ft); break;

    case Op::cvt_w_s: cvt_w_s(fpu, fd, fs); break;
    case Op::cvt_s_w: cvt_s_w(fpu, fd, fs); break;

    case Op::c_f_s: c_f_s(fpu); break;
    case Op::c_eq_s: c_eq_s(fpu, fs, ft); break;
    case Op::c_lt_s: c_lt_s(fpu, fs, ft); break;
    case Op::c_le_s: c_le_s(fpu, fs, ft); break;

    default:
        return false;
    }
    return true;
}

}  // namespace uee
