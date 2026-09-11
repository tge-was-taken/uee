#include "vu0_interpreter.h"

#include "instruction.h"
#include "vu_handlers.h"

namespace uee {

bool executeVu0(Vu& vu0, Memory& memory, Op op, Instruction insn) {
    vu0.beginMacro();
    const u32 dest = insn.dest();

    // The three that fill the quotient register hand it over late.
    const auto delayed = [&vu0](auto&& run) {
        const u32 previous = vu0.vi[Vu::kQ];
        run();
        vu0.delayQuotient(previous);
    };

    switch (op) {
    case Op::vmove: vmove(vu0, dest, insn.vt(), insn.vs()); break;
    case Op::vmr32: vmr32(vu0, dest, insn.vt(), insn.vs()); break;
    case Op::vabs: vabs(vu0, dest, insn.vt(), insn.vs()); break;
    case Op::vitof0: vitof(vu0, dest, insn.vt(), insn.vs(), 0); break;
    case Op::vitof4: vitof(vu0, dest, insn.vt(), insn.vs(), 4); break;
    case Op::vitof12: vitof(vu0, dest, insn.vt(), insn.vs(), 12); break;
    case Op::vitof15: vitof(vu0, dest, insn.vt(), insn.vs(), 15); break;
    case Op::vftoi0: vftoi(vu0, dest, insn.vt(), insn.vs(), 0); break;
    case Op::vftoi4: vftoi(vu0, dest, insn.vt(), insn.vs(), 4); break;
    case Op::vftoi12: vftoi(vu0, dest, insn.vt(), insn.vs(), 12); break;
    case Op::vftoi15: vftoi(vu0, dest, insn.vt(), insn.vs(), 15); break;

    case Op::vnop: break;
    case Op::vwaitq: vu0.syncQuotient(); break;

    case Op::viadd: viadd(vu0, insn.vid(), insn.vis(), insn.vit()); break;
    case Op::visub: visub(vu0, insn.vid(), insn.vis(), insn.vit()); break;
    case Op::viand: viand(vu0, insn.vid(), insn.vis(), insn.vit()); break;
    case Op::vior: vior(vu0, insn.vid(), insn.vis(), insn.vit()); break;
    case Op::viaddi: viaddi(vu0, insn.vit(), insn.vis(), insn.imm5()); break;

    case Op::vilwr:
        vilwr(vu0, memory, dest, insn.vit(), insn.vis(), kVu0DataBase, kVu0MemorySize);
        break;
    case Op::viswr:
        viswr(vu0, memory, dest, insn.vit(), insn.vis(), kVu0DataBase, kVu0MemorySize);
        break;
    case Op::vsqi: vsqi(vu0, memory, dest, insn.vs(), insn.vit()); break;

    case Op::vclipw:
        vclip(vu0, insn.vs(), insn.vt());
        // Macro mode reads the running value straight, where a microprogram
        // takes it from a delay line.
        vu0.vi[Vu::kClip] = vu0.clip;
        break;
    case Op::vdiv:
        delayed([&] { vdiv(vu0, insn.vs(), insn.fsf(), insn.vt(), insn.ftf()); });
        break;
    case Op::vsqrt: delayed([&] { vsqrt(vu0, insn.vt(), insn.ftf()); }); break;
    case Op::vrsqrt:
        delayed([&] { vrsqrt(vu0, insn.vs(), insn.fsf(), insn.vt(), insn.ftf()); });
        break;

    case Op::vcallms: vcallms(vu0, memory, insn.imm15()); break;
    case Op::vcallmsr: vcallmsr(vu0, memory); break;

    case Op::vopmula: vopmula(vu0, dest, insn.vs(), insn.vt()); break;
    case Op::vopmsub: vopmsub(vu0, dest, insn.vd(), insn.vs(), insn.vt()); break;

    default: {
        VuForm form;
        if (!findVuForm(op, form)) {
            return false;
        }
        vectorOp(vu0, form, dest, insn.vd(), insn.vs(), insn.vt());
        break;
    }
    }
    vu0.endMacro();
    return true;
}

}  // namespace uee
