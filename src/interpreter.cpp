#include "interpreter.h"

#include "handlers.h"

namespace uee {

Status Interpreter::execute(Instruction insn) {
    R5900& cpu = ee.cpu;
    Memory& memory = mMemory;
    const Slot& slot = decode(insn);
    const u32 rs = insn.rs();
    const u32 rt = insn.rt();
    const u32 rd = insn.rd();
    const u32 sa = insn.sa();
    const u32 imm = insn.imm();
    const s32 simm = insn.simm();
    const u32 pc = cpu.pc;
    const u32 target = branchTarget(insn, pc);
    // a branch resolves the PC itself; everything else falls through to pc + 4
    cpu.pc = pc + 4;
    mDelaySlot = false;
    mBranchTaken = false;
    countPerformanceEvents(cpu, 1);

    const auto branchIf = [&](bool taken) {
        cpu.pc = taken ? target : pc + 8;
        mDelaySlot = true;
        mBranchTaken = taken;
    };
    const auto branchLikelyIf = [&](bool taken) {
        cpu.pc = taken ? target : pc + 8;
        mDelaySlot = taken;
        mBranchTaken = taken;
    };
    const auto jumpTo = [&](u32 destination) {
        cpu.pc = destination;
        mDelaySlot = true;
        mBranchTaken = true;
    };
    // the link register holds the 32-bit return address unextended
    const auto link = [&](u32 reg) { cpu.write64(reg, pc + 8); };
    // The condition is read before the link register is written, which is the
    // difference when they name the same register.
    const auto linkAndBranchIf = [&](bool taken) {
        link(reg::ra);
        branchIf(taken);
    };
    const auto linkAndBranchLikelyIf = [&](bool taken) {
        link(reg::ra);
        branchLikelyIf(taken);
    };

    switch (slot.op) {
        case Op::Invalid:
            return Status::Undecodable;

        case Op::sll: sll(cpu, rd, rt, sa); break;
        case Op::srl: srl(cpu, rd, rt, sa); break;
        case Op::sra: sra(cpu, rd, rt, sa); break;
        case Op::dsll: dsll(cpu, rd, rt, sa); break;
        case Op::dsrl: dsrl(cpu, rd, rt, sa); break;
        case Op::dsra: dsra(cpu, rd, rt, sa); break;
        case Op::dsll32: dsll32(cpu, rd, rt, sa); break;
        case Op::dsrl32: dsrl32(cpu, rd, rt, sa); break;
        case Op::dsra32: dsra32(cpu, rd, rt, sa); break;

        case Op::sllv: sllv(cpu, rd, rt, rs); break;
        case Op::srlv: srlv(cpu, rd, rt, rs); break;
        case Op::srav: srav(cpu, rd, rt, rs); break;
        case Op::dsllv: dsllv(cpu, rd, rt, rs); break;
        case Op::dsrlv: dsrlv(cpu, rd, rt, rs); break;
        case Op::dsrav: dsrav(cpu, rd, rt, rs); break;

        case Op::add:
        case Op::addu: addu(cpu, rd, rs, rt); break;
        case Op::sub:
        case Op::subu: subu(cpu, rd, rs, rt); break;
        case Op::dadd:
        case Op::daddu: daddu(cpu, rd, rs, rt); break;
        case Op::dsub:
        case Op::dsubu: dsubu(cpu, rd, rs, rt); break;
        case Op::and_: and_(cpu, rd, rs, rt); break;
        case Op::or_: or_(cpu, rd, rs, rt); break;
        case Op::xor_: xor_(cpu, rd, rs, rt); break;
        case Op::nor: nor(cpu, rd, rs, rt); break;
        case Op::slt: slt(cpu, rd, rs, rt); break;
        case Op::sltu: sltu(cpu, rd, rs, rt); break;

        case Op::addi:
        case Op::addiu: addiu(cpu, rt, rs, simm); break;
        case Op::daddi:
        case Op::daddiu: daddiu(cpu, rt, rs, simm); break;
        case Op::andi: andi(cpu, rt, rs, imm); break;
        case Op::ori: ori(cpu, rt, rs, imm); break;
        case Op::xori: xori(cpu, rt, rs, imm); break;
        case Op::slti: slti(cpu, rt, rs, simm); break;
        case Op::sltiu: sltiu(cpu, rt, rs, simm); break;
        case Op::lui: lui(cpu, rt, imm); break;

        case Op::pand: pand(cpu, rd, rs, rt); break;
        case Op::por: por(cpu, rd, rs, rt); break;
        case Op::pxor: pxor(cpu, rd, rs, rt); break;
        case Op::pnor: pnor(cpu, rd, rs, rt); break;
        case Op::pcpyld: pcpyld(cpu, rd, rs, rt); break;
        case Op::pcpyud: pcpyud(cpu, rd, rs, rt); break;
        case Op::pextlw: pextlw(cpu, rd, rs, rt); break;
        case Op::pextuw: pextuw(cpu, rd, rs, rt); break;
        case Op::paddw: paddw(cpu, rd, rs, rt); break;
        case Op::psubw: psubw(cpu, rd, rs, rt); break;
        case Op::psubb: psubb(cpu, rd, rs, rt); break;
        case Op::paddub: paddub(cpu, rd, rs, rt); break;
        case Op::padduw: padduw(cpu, rd, rs, rt); break;
        case Op::pextlb: pextlb(cpu, rd, rs, rt); break;
        case Op::pextlh: pextlh(cpu, rd, rs, rt); break;
        case Op::ppacb: ppacb(cpu, rd, rs, rt); break;
        case Op::ppach: ppach(cpu, rd, rs, rt); break;
        case Op::ppacw: ppacw(cpu, rd, rs, rt); break;
        case Op::pexew: pexew(cpu, rd, rt); break;
        case Op::prot3w: prot3w(cpu, rd, rt); break;
        case Op::pcpyh: pcpyh(cpu, rd, rt); break;
        case Op::plzcw: plzcw(cpu, rd, rs); break;
        case Op::paddb: paddb(cpu, rd, rs, rt); break;
        case Op::paddh: paddh(cpu, rd, rs, rt); break;
        case Op::psubh: psubh(cpu, rd, rs, rt); break;
        case Op::paddsb: paddsb(cpu, rd, rs, rt); break;
        case Op::paddsh: paddsh(cpu, rd, rs, rt); break;
        case Op::paddsw: paddsw(cpu, rd, rs, rt); break;
        case Op::psubsb: psubsb(cpu, rd, rs, rt); break;
        case Op::psubsh: psubsh(cpu, rd, rs, rt); break;
        case Op::psubsw: psubsw(cpu, rd, rs, rt); break;
        case Op::padduh: padduh(cpu, rd, rs, rt); break;
        case Op::psubub: psubub(cpu, rd, rs, rt); break;
        case Op::psubuh: psubuh(cpu, rd, rs, rt); break;
        case Op::psubuw: psubuw(cpu, rd, rs, rt); break;
        case Op::pmaxh: pmaxh(cpu, rd, rs, rt); break;
        case Op::pminh: pminh(cpu, rd, rs, rt); break;
        case Op::pmaxw: pmaxw(cpu, rd, rs, rt); break;
        case Op::pminw: pminw(cpu, rd, rs, rt); break;
        case Op::pabsh: pabsh(cpu, rd, rt); break;
        case Op::pabsw: pabsw(cpu, rd, rt); break;
        case Op::pceqb: pceqb(cpu, rd, rs, rt); break;
        case Op::pceqh: pceqh(cpu, rd, rs, rt); break;
        case Op::pceqw: pceqw(cpu, rd, rs, rt); break;
        case Op::pcgtb: pcgtb(cpu, rd, rs, rt); break;
        case Op::pcgth: pcgth(cpu, rd, rs, rt); break;
        case Op::pcgtw: pcgtw(cpu, rd, rs, rt); break;
        case Op::psllh: psllh(cpu, rd, rt, sa); break;
        case Op::psrlh: psrlh(cpu, rd, rt, sa); break;
        case Op::psrah: psrah(cpu, rd, rt, sa); break;
        case Op::psllw: psllw(cpu, rd, rt, sa); break;
        case Op::psrlw: psrlw(cpu, rd, rt, sa); break;
        case Op::psraw: psraw(cpu, rd, rt, sa); break;
        case Op::psllvw: psllvw(cpu, rd, rt, rs); break;
        case Op::psrlvw: psrlvw(cpu, rd, rt, rs); break;
        case Op::psravw: psravw(cpu, rd, rt, rs); break;
        case Op::qfsrv: qfsrv(cpu, rd, rs, rt); break;
        case Op::pextub: pextub(cpu, rd, rs, rt); break;
        case Op::pextuh: pextuh(cpu, rd, rs, rt); break;
        case Op::pinth: pinth(cpu, rd, rs, rt); break;
        case Op::pinteh: pinteh(cpu, rd, rs, rt); break;
        case Op::prevh: prevh(cpu, rd, rt); break;
        case Op::pexeh: pexeh(cpu, rd, rt); break;
        case Op::pexch: pexch(cpu, rd, rt); break;
        case Op::pexcw: pexcw(cpu, rd, rt); break;
        case Op::pext5: pext5(cpu, rd, rt); break;
        case Op::ppac5: ppac5(cpu, rd, rt); break;
        case Op::padsbh: padsbh(cpu, rd, rs, rt); break;

        case Op::mult: mult(cpu, rd, rs, rt, false); break;
        case Op::multu: multu(cpu, rd, rs, rt, false); break;
        case Op::mult1: mult(cpu, rd, rs, rt, true); break;
        case Op::multu1: multu(cpu, rd, rs, rt, true); break;
        case Op::madd: madd(cpu, rd, rs, rt, false); break;
        case Op::madd1: madd(cpu, rd, rs, rt, true); break;
        case Op::maddu: maddu(cpu, rd, rs, rt, false); break;
        case Op::maddu1: maddu(cpu, rd, rs, rt, true); break;
        case Op::pmultw: pmultw(cpu, rd, rs, rt); break;
        case Op::pmultuw: pmultuw(cpu, rd, rs, rt); break;
        case Op::pmaddw: pmaddw(cpu, rd, rs, rt); break;
        case Op::pmadduw: pmadduw(cpu, rd, rs, rt); break;
        case Op::pmsubw: pmsubw(cpu, rd, rs, rt); break;
        case Op::pmulth: pmulth(cpu, rd, rs, rt); break;
        case Op::pmaddh: pmaddh(cpu, rd, rs, rt); break;
        case Op::pmsubh: pmsubh(cpu, rd, rs, rt); break;
        case Op::phmadh: phmadh(cpu, rd, rs, rt); break;
        case Op::phmsbh: phmsbh(cpu, rd, rs, rt); break;
        case Op::pdivw: pdivw(cpu, rs, rt); break;
        case Op::pdivuw: pdivuw(cpu, rs, rt); break;
        case Op::pdivbw: pdivbw(cpu, rs, rt); break;
        case Op::div: div(cpu, rs, rt, false); break;
        case Op::divu: divu(cpu, rs, rt, false); break;
        case Op::div1: div(cpu, rs, rt, true); break;
        case Op::divu1: divu(cpu, rs, rt, true); break;

        case Op::mthi: mthi(cpu, rs, false); break;
        case Op::mtlo: mtlo(cpu, rs, false); break;
        case Op::mthi1: mthi(cpu, rs, true); break;
        case Op::mtlo1: mtlo(cpu, rs, true); break;
        case Op::mfhi: mfhi(cpu, rd, false); break;
        case Op::mflo: mflo(cpu, rd, false); break;
        case Op::mfhi1: mfhi(cpu, rd, true); break;
        case Op::mflo1: mflo(cpu, rd, true); break;
        case Op::pmfhi: pmfhi(cpu, rd); break;
        case Op::pmflo: pmflo(cpu, rd); break;
        case Op::pmfhl_lw: pmfhl_lw(cpu, rd); break;
        case Op::pmfhl_uw: pmfhl_uw(cpu, rd); break;
        case Op::pmfhl_slw: pmfhl_slw(cpu, rd); break;
        case Op::pmfhl_lh: pmfhl_lh(cpu, rd); break;
        case Op::pmfhl_sh: pmfhl_sh(cpu, rd); break;
        case Op::pmthl_lw: pmthl_lw(cpu, rs); break;
        case Op::pmthi: pmthi(cpu, rs); break;
        case Op::pmtlo: pmtlo(cpu, rs); break;

        case Op::movz: movz(cpu, rd, rs, rt); break;
        case Op::movn: movn(cpu, rd, rs, rt); break;

        case Op::mfsa: mfsa(cpu, rd); break;
        case Op::mtsa: mtsa(cpu, rs); break;
        case Op::mtsab: mtsab(cpu, rs, imm); break;
        case Op::mtsah: mtsah(cpu, rs, imm); break;

        case Op::beq: branchIf(beqTaken(cpu, rs, rt)); break;
        case Op::beql: branchLikelyIf(beqTaken(cpu, rs, rt)); break;
        case Op::bne: branchIf(bneTaken(cpu, rs, rt)); break;
        case Op::bnel: branchLikelyIf(bneTaken(cpu, rs, rt)); break;
        case Op::blez: branchIf(blezTaken(cpu, rs)); break;
        case Op::blezl: branchLikelyIf(blezTaken(cpu, rs)); break;
        case Op::bgtz: branchIf(bgtzTaken(cpu, rs)); break;
        case Op::bgtzl: branchLikelyIf(bgtzTaken(cpu, rs)); break;
        case Op::bltz: branchIf(bltzTaken(cpu, rs)); break;
        case Op::bltzl: branchLikelyIf(bltzTaken(cpu, rs)); break;
        case Op::bgez: branchIf(bgezTaken(cpu, rs)); break;
        case Op::bgezl: branchLikelyIf(bgezTaken(cpu, rs)); break;
        case Op::bltzal: linkAndBranchIf(bltzTaken(cpu, rs)); break;
        case Op::bltzall: linkAndBranchLikelyIf(bltzTaken(cpu, rs)); break;
        case Op::bgezal: linkAndBranchIf(bgezTaken(cpu, rs)); break;
        case Op::bgezall: linkAndBranchLikelyIf(bgezTaken(cpu, rs)); break;
        case Op::bc1f: branchIf(!bc1tTaken(ee.fpu)); break;
        case Op::bc1fl: branchLikelyIf(!bc1tTaken(ee.fpu)); break;
        case Op::bc1t: branchIf(bc1tTaken(ee.fpu)); break;
        case Op::bc1tl: branchLikelyIf(bc1tTaken(ee.fpu)); break;
        case Op::j: jumpTo(jumpTarget(insn, pc)); break;
        case Op::jal:
            link(31);
            jumpTo(jumpTarget(insn, pc));
            break;
        case Op::jr: jumpTo(low(cpu.gpr[rs])); break;
        case Op::jalr: {
            const u32 destination = low(cpu.gpr[rs]);
            link(rd);
            jumpTo(destination);
            break;
        }

        case Op::lb: lb(cpu, memory, rt, rs, simm); break;
        case Op::lbu: lbu(cpu, memory, rt, rs, simm); break;
        case Op::lh: lh(cpu, memory, rt, rs, simm); break;
        case Op::lhu: lhu(cpu, memory, rt, rs, simm); break;
        case Op::lw: lw(cpu, memory, rt, rs, simm); break;
        case Op::lwu: lwu(cpu, memory, rt, rs, simm); break;
        case Op::ld: ld(cpu, memory, rt, rs, simm); break;
        case Op::lq: lq(cpu, memory, rt, rs, simm); break;
        case Op::lwc1: lwc1(cpu, ee.fpu, memory, rt, rs, simm); break;
        case Op::lqc2: lqc2(cpu, ee.vu0, memory, rt, rs, simm); break;

        case Op::sb: sb(cpu, memory, rt, rs, simm); break;
        case Op::sh: sh(cpu, memory, rt, rs, simm); break;
        case Op::sw: sw(cpu, memory, rt, rs, simm); break;
        case Op::sd: sd(cpu, memory, rt, rs, simm); break;
        case Op::sq: sq(cpu, memory, rt, rs, simm); break;
        case Op::swc1: swc1(cpu, ee.fpu, memory, rt, rs, simm); break;
        case Op::sqc2: sqc2(cpu, ee.vu0, memory, rt, rs, simm); break;

        case Op::lwl: lwl(cpu, memory, rt, rs, simm); break;
        case Op::lwr: lwr(cpu, memory, rt, rs, simm); break;
        case Op::ldl: ldl(cpu, memory, rt, rs, simm); break;
        case Op::ldr: ldr(cpu, memory, rt, rs, simm); break;
        case Op::swl: swl(cpu, memory, rt, rs, simm); break;
        case Op::swr: swr(cpu, memory, rt, rs, simm); break;
        case Op::sdl: sdl(cpu, memory, rt, rs, simm); break;
        case Op::sdr: sdr(cpu, memory, rt, rs, simm); break;

        case Op::add_s: case Op::sub_s: case Op::mul_s: case Op::div_s:
        case Op::sqrt_s: case Op::abs_s: case Op::neg_s: case Op::mov_s:
        case Op::cvt_w_s: case Op::cvt_s_w:
        case Op::max_s: case Op::min_s: case Op::rsqrt_s:
        case Op::c_f_s: case Op::c_eq_s: case Op::c_lt_s: case Op::c_le_s:
        case Op::adda_s: case Op::suba_s: case Op::mula_s:
        case Op::madda_s: case Op::msuba_s: case Op::madd_s: case Op::msub_s:
            if (!mFpu.execute(slot.op, insn)) {
                return Status::Unimplemented;
            }
            break;

        case Op::mfc0: mfc0(cpu, rt, rd, insn.word & 0x3Fu); break;
        case Op::mtc0: mtc0(cpu, rt, rd, insn.word & 0x3Fu); break;
        case Op::di: di(cpu); break;
        case Op::ei: ei(cpu); break;
        case Op::bc0f: branchIf(!cpu.cop0condition); break;
        case Op::bc0fl: branchLikelyIf(!cpu.cop0condition); break;
        case Op::bc0t: branchIf(cpu.cop0condition); break;
        case Op::bc0tl: branchLikelyIf(cpu.cop0condition); break;
        case Op::syscall: raiseException(cpu, pc, cop0::kSystemCall); break;
        case Op::break_: raiseException(cpu, pc, cop0::kBreakpoint); break;
        case Op::teq: if (teqTaken(cpu, rs, rt)) return Status::Unimplemented; break;
        case Op::tne: if (tneTaken(cpu, rs, rt)) return Status::Unimplemented; break;
        case Op::tge: if (tgeTaken(cpu, rs, rt)) return Status::Unimplemented; break;
        case Op::tgeu: if (tgeuTaken(cpu, rs, rt)) return Status::Unimplemented; break;
        case Op::tlt: if (tltTaken(cpu, rs, rt)) return Status::Unimplemented; break;
        case Op::tltu: if (tltuTaken(cpu, rs, rt)) return Status::Unimplemented; break;
        case Op::teqi: if (teqiTaken(cpu, rs, insn.simm())) return Status::Unimplemented; break;
        case Op::tnei: if (tneiTaken(cpu, rs, insn.simm())) return Status::Unimplemented; break;
        case Op::tgei: if (tgeiTaken(cpu, rs, insn.simm())) return Status::Unimplemented; break;
        case Op::tgeiu: if (tgeiuTaken(cpu, rs, insn.simm())) return Status::Unimplemented; break;
        case Op::tlti: if (tltiTaken(cpu, rs, insn.simm())) return Status::Unimplemented; break;
        case Op::tltiu: if (tltiuTaken(cpu, rs, insn.simm())) return Status::Unimplemented; break;

        case Op::eret: returnFromException(cpu); break;

        // no architectural state this models
        case Op::sync_p:
        case Op::cache:
        case Op::pref:
        case Op::tlbwi:
        case Op::tlbwr:
        case Op::tlbr:
        case Op::tlbp:
            break;

        case Op::mfc1: mfc1(cpu, ee.fpu, rt, insn.fs()); break;
        case Op::mtc1: mtc1(cpu, ee.fpu, rt, insn.fs()); break;
        case Op::cfc1: cfc1(cpu, ee.fpu, rt, insn.fs()); break;
        case Op::ctc1: ctc1(cpu, ee.fpu, rt, insn.fs()); break;

        case Op::qmfc2_ni: qmfc2(cpu, ee.vu0, rt, insn.vs()); break;
        case Op::qmtc2_ni: qmtc2(cpu, ee.vu0, rt, insn.vs()); break;
        case Op::cfc2_ni: cfc2(cpu, ee.vu0, rt, rd); break;
        case Op::ctc2_ni: ctc2(cpu, ee.vu0, rt, rd); break;

        default:
            if (!mVu0.execute(slot.op, insn)) {
                return Status::Unimplemented;
            }
            break;
    }
    return Status::Executed;
}

}  // namespace uee
