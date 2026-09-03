#include "iop_interpreter.h"

#include "iop_handlers.h"

namespace uee {

Status IopInterpreter::execute(Instruction insn) {
    R3000A& cpu = iop.cpu;
    Memory& memory = mMemory;
    const Slot& slot = decode(insn);
    if (slot.op == Op::Invalid) {
        return Status::Undecodable;
    }

    const u32 rs = insn.rs();
    const u32 rt = insn.rt();
    const u32 rd = insn.rd();
    const u32 sa = insn.sa();
    const u32 imm = insn.imm();
    const s32 simm = insn.simm();
    const u32 pc = cpu.pc;
    const u32 target = branchTarget(insn, pc);
    cpu.pc = pc + 4;
    mDelaySlot = false;
    mBranchTaken = false;
    commitLoad(cpu);

    const auto branchIf = [&](bool taken) {
        cpu.pc = taken ? target : pc + 8;
        mDelaySlot = true;
        mBranchTaken = taken;
    };
    const auto jumpTo = [&](u32 destination) {
        cpu.pc = destination;
        mDelaySlot = true;
        mBranchTaken = true;
    };
    const auto link = [&](u32 reg) { cpu.write(reg, pc + 8); };
    // The condition is read before the link register is written.
    const auto linkAndBranchIf = [&](bool taken) {
        link(reg::ra);
        branchIf(taken);
    };

    switch (slot.op) {
        case Op::sll: sll(cpu, rd, rt, sa); break;
        case Op::srl: srl(cpu, rd, rt, sa); break;
        case Op::sra: sra(cpu, rd, rt, sa); break;
        case Op::sllv: sllv(cpu, rd, rt, rs); break;
        case Op::srlv: srlv(cpu, rd, rt, rs); break;
        case Op::srav: srav(cpu, rd, rt, rs); break;

        case Op::add:
        case Op::addu: addu(cpu, rd, rs, rt); break;
        case Op::sub:
        case Op::subu: subu(cpu, rd, rs, rt); break;
        case Op::and_: and_(cpu, rd, rs, rt); break;
        case Op::or_: or_(cpu, rd, rs, rt); break;
        case Op::xor_: xor_(cpu, rd, rs, rt); break;
        case Op::nor: nor(cpu, rd, rs, rt); break;
        case Op::slt: slt(cpu, rd, rs, rt); break;
        case Op::sltu: sltu(cpu, rd, rs, rt); break;

        case Op::addi:
        case Op::addiu: addiu(cpu, rt, rs, simm); break;
        case Op::andi: andi(cpu, rt, rs, imm); break;
        case Op::ori: ori(cpu, rt, rs, imm); break;
        case Op::xori: xori(cpu, rt, rs, imm); break;
        case Op::slti: slti(cpu, rt, rs, simm); break;
        case Op::sltiu: sltiu(cpu, rt, rs, simm); break;
        case Op::lui: lui(cpu, rt, imm); break;

        case Op::mult: mult(cpu, rs, rt); break;
        case Op::multu: multu(cpu, rs, rt); break;
        case Op::div: div(cpu, rs, rt); break;
        case Op::divu: divu(cpu, rs, rt); break;
        case Op::mfhi: mfhi(cpu, rd); break;
        case Op::mflo: mflo(cpu, rd); break;
        case Op::mthi: mthi(cpu, rs); break;
        case Op::mtlo: mtlo(cpu, rs); break;

        case Op::beq: branchIf(beqTaken(cpu, rs, rt)); break;
        case Op::bne: branchIf(bneTaken(cpu, rs, rt)); break;
        case Op::blez: branchIf(blezTaken(cpu, rs)); break;
        case Op::bgtz: branchIf(bgtzTaken(cpu, rs)); break;
        case Op::bltz: branchIf(bltzTaken(cpu, rs)); break;
        case Op::bgez: branchIf(bgezTaken(cpu, rs)); break;
        case Op::bltzal: linkAndBranchIf(bltzTaken(cpu, rs)); break;
        case Op::bgezal: linkAndBranchIf(bgezTaken(cpu, rs)); break;
        case Op::j: jumpTo(jumpTarget(insn, pc)); break;
        case Op::jal:
            link(reg::ra);
            jumpTo(jumpTarget(insn, pc));
            break;
        case Op::jr: jumpTo(cpu.gpr[rs]); break;
        case Op::jalr: {
            const u32 destination = cpu.gpr[rs];
            link(rd);
            jumpTo(destination);
            break;
        }

        case Op::lb: lb(cpu, memory, rt, rs, simm); break;
        case Op::lbu: lbu(cpu, memory, rt, rs, simm); break;
        case Op::lh: lh(cpu, memory, rt, rs, simm); break;
        case Op::lhu: lhu(cpu, memory, rt, rs, simm); break;
        case Op::lw: lw(cpu, memory, rt, rs, simm); break;
        case Op::lwl: lwl(cpu, memory, rt, rs, simm); break;
        case Op::lwr: lwr(cpu, memory, rt, rs, simm); break;

        case Op::sb: sb(cpu, memory, rt, rs, simm); break;
        case Op::sh: sh(cpu, memory, rt, rs, simm); break;
        case Op::sw: sw(cpu, memory, rt, rs, simm); break;
        case Op::swl: swl(cpu, memory, rt, rs, simm); break;
        case Op::swr: swr(cpu, memory, rt, rs, simm); break;

        case Op::mfc0: mfc0(cpu, rt, rd); break;
        case Op::mtc0: mtc0(cpu, rt, rd); break;
        case Op::rfe: rfe(cpu); break;
        case Op::break_: raiseException(cpu, pc, r3000::kBreakpoint); break;

        default:
            return Status::Unimplemented;
    }
    return Status::Executed;
}

}  // namespace uee
