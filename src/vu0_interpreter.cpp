#include "vu0_interpreter.h"

#include "instruction.h"
#include "vu0_handlers.h"

namespace uee {

bool executeVu0(Vu0& vu0, Memory& memory, Op op, Instruction insn) {
    const u32 dest = insn.dest();

    switch (op) {
    case Op::vmove: vmove(vu0, dest, insn.vt(), insn.vs()); break;
    case Op::vabs: vabs(vu0, dest, insn.vt(), insn.vs()); break;
    case Op::vitof0: vitof0(vu0, dest, insn.vt(), insn.vs()); break;
    case Op::vftoi0: vftoi0(vu0, dest, insn.vt(), insn.vs()); break;

    case Op::vnop:
    case Op::vwaitq:
        break;

    case Op::viadd: viadd(vu0, insn.vid(), insn.vis(), insn.vit()); break;
    case Op::visub: visub(vu0, insn.vid(), insn.vis(), insn.vit()); break;
    case Op::viand: viand(vu0, insn.vid(), insn.vis(), insn.vit()); break;
    case Op::vior: vior(vu0, insn.vid(), insn.vis(), insn.vit()); break;
    case Op::viaddi: viaddi(vu0, insn.vit(), insn.vis(), insn.imm5()); break;

    case Op::vilwr: vilwr(vu0, memory, dest, insn.vit(), insn.vis()); break;
    case Op::viswr: viswr(vu0, memory, dest, insn.vit(), insn.vis()); break;
    case Op::vsqi: vsqi(vu0, memory, dest, insn.vs(), insn.vit()); break;

    case Op::vsqrt: vsqrt(vu0, insn.vt(), insn.ftf()); break;
    case Op::vrsqrt: vrsqrt(vu0, insn.vs(), insn.fsf(), insn.vt(), insn.ftf()); break;

    case Op::vopmula: vopmula(vu0, dest, insn.vs(), insn.vt()); break;
    case Op::vopmsub: vopmsub(vu0, dest, insn.vd(), insn.vs(), insn.vt()); break;

    default: {
        VuForm form;
        if (!vuFormOf(op, form)) {
            return false;
        }
        vectorOp(vu0, form, dest, insn.vd(), insn.vs(), insn.vt());
        break;
    }
    }
    return true;
}

}  // namespace uee
