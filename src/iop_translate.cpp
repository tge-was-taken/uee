#include "iop_translate.h"

#include "irx.h"
#include "translator.h"

#include <format>
#include <optional>
#include <string>

namespace uee {
namespace {

class IopTranslator final : public Translator {
public:
    using Translator::Translator;

private:
    std::string_view getMachineType() const override { return "IopMachine"; }
    std::string_view getContextType() const override { return "IopContext"; }
    std::string_view getStateName() const override { return "iop"; }
    std::string_view getHandlersHeader() const override { return "iop_handlers.h"; }
    std::string_view getRuntimeHeader() const override { return "iop_translate.h"; }
    std::string emitBeforeInstruction() const override { return "commitLoad(iop.cpu);"; }

    std::string readGpr(u32 reg) const override;
    std::string writeGpr(u32 reg, u32 value) const override;
    std::optional<std::string> emitOne(Instruction insn) const override;
    std::optional<std::string> emitBranchCondition(Op op, Instruction insn) const override;
    std::optional<Trap> emitTrap(Instruction insn, Instruction previous, u32 pc) const override;
};

std::optional<std::string> IopTranslator::emitOne(Instruction insn) const {
    const u32 rs = insn.rs();
    const u32 rt = insn.rt();
    const u32 rd = insn.rd();
    const u32 sa = insn.sa();
    const u32 imm = insn.imm();
    const s32 simm = insn.simm();
    const auto r = [](u32 i) { return regName(i); };

    // sll zero, zero, 0 is the canonical nop.
    if (insn.word == 0) {
        return std::string{};
    }

    const auto gpr3 = [&](const char* name, u32 a, u32 b, u32 c) {
        return std::format("{}(iop.cpu, {}, {}, {});", name, r(a), r(b), r(c));
    };
    const auto gpr2 = [&](const char* name, u32 a, u32 b) {
        return std::format("{}(iop.cpu, {}, {});", name, r(a), r(b));
    };
    const auto gpr1 = [&](const char* name, u32 a) {
        return std::format("{}(iop.cpu, {});", name, r(a));
    };
    const auto shift = [&](const char* name, u32 a, u32 b, u32 amount) {
        return std::format("{}(iop.cpu, {}, {}, {});", name, r(a), r(b), amount);
    };
    const auto withImm = [&](const char* name, u32 a, u32 b, auto value) {
        return std::format("{}(iop.cpu, {}, {}, {});", name, r(a), r(b), value);
    };
    const auto access = [&](const char* name, u32 a, u32 base, s32 offset) {
        return std::format("{}(iop.cpu, mem, {}, {}, {});", name, r(a), r(base), offset);
    };

    switch (opOf(insn)) {
        case Op::sll: return shift("sll", rd, rt, sa);
        case Op::srl: return shift("srl", rd, rt, sa);
        case Op::sra: return shift("sra", rd, rt, sa);
        case Op::sllv: return gpr3("sllv", rd, rt, rs);
        case Op::srlv: return gpr3("srlv", rd, rt, rs);
        case Op::srav: return gpr3("srav", rd, rt, rs);

        case Op::add:
        case Op::addu: return gpr3("addu", rd, rs, rt);
        case Op::sub:
        case Op::subu: return gpr3("subu", rd, rs, rt);
        case Op::and_: return gpr3("and_", rd, rs, rt);
        case Op::or_: return gpr3("or_", rd, rs, rt);
        case Op::xor_: return gpr3("xor_", rd, rs, rt);
        case Op::nor: return gpr3("nor", rd, rs, rt);
        case Op::slt: return gpr3("slt", rd, rs, rt);
        case Op::sltu: return gpr3("sltu", rd, rs, rt);

        case Op::addi:
        case Op::addiu: return withImm("addiu", rt, rs, simm);
        case Op::andi: return withImm("andi", rt, rs, imm);
        case Op::ori: return withImm("ori", rt, rs, imm);
        case Op::xori: return withImm("xori", rt, rs, imm);
        case Op::slti: return withImm("slti", rt, rs, simm);
        case Op::sltiu: return withImm("sltiu", rt, rs, simm);
        case Op::lui: return std::format("lui(iop.cpu, {}, {});", r(rt), imm);

        case Op::mult: return gpr2("mult", rs, rt);
        case Op::multu: return gpr2("multu", rs, rt);
        case Op::div: return gpr2("div", rs, rt);
        case Op::divu: return gpr2("divu", rs, rt);
        case Op::mfhi: return gpr1("mfhi", rd);
        case Op::mflo: return gpr1("mflo", rd);
        case Op::mthi: return gpr1("mthi", rs);
        case Op::mtlo: return gpr1("mtlo", rs);

        case Op::lb: return access("lb", rt, rs, simm);
        case Op::lbu: return access("lbu", rt, rs, simm);
        case Op::lh: return access("lh", rt, rs, simm);
        case Op::lhu: return access("lhu", rt, rs, simm);
        case Op::lw: return access("lw", rt, rs, simm);
        case Op::lwl: return access("lwl", rt, rs, simm);
        case Op::lwr: return access("lwr", rt, rs, simm);
        case Op::sb: return access("sb", rt, rs, simm);
        case Op::sh: return access("sh", rt, rs, simm);
        case Op::sw: return access("sw", rt, rs, simm);
        case Op::swl: return access("swl", rt, rs, simm);
        case Op::swr: return access("swr", rt, rs, simm);

        case Op::mfc0: return std::format("mfc0(iop.cpu, {}, {});", r(rt), rd);
        case Op::mtc0: return std::format("mtc0(iop.cpu, {}, {});", r(rt), rd);
        case Op::rfe: return "rfe(iop.cpu);";

        default: return std::nullopt;
    }
}

std::optional<std::string> IopTranslator::emitBranchCondition(Op op, Instruction insn) const {
    const auto r = [](u32 i) { return regName(i); };
    const u32 rs = insn.rs();
    const u32 rt = insn.rt();
    switch (op) {
        case Op::beq: return std::format("beqTaken(iop.cpu, {}, {})", r(rs), r(rt));
        case Op::bne: return std::format("bneTaken(iop.cpu, {}, {})", r(rs), r(rt));
        case Op::blez: return std::format("blezTaken(iop.cpu, {})", r(rs));
        case Op::bgtz: return std::format("bgtzTaken(iop.cpu, {})", r(rs));
        case Op::bltz: case Op::bltzal: return std::format("bltzTaken(iop.cpu, {})", r(rs));
        case Op::bgez: case Op::bgezal: return std::format("bgezTaken(iop.cpu, {})", r(rs));
        default: return std::nullopt;
    }
}

std::string IopTranslator::readGpr(u32 reg) const {
    return std::format("iop.cpu.gpr[{}]", regName(reg));
}

std::string IopTranslator::writeGpr(u32 reg, u32 value) const {
    return std::format("iop.cpu.write({}, 0x{:08x}u);", regName(reg), value);
}

// The only syscalls a module carries are import stubs, which resolve the pc
// themselves.
std::optional<Trap> IopTranslator::emitTrap(Instruction insn, Instruction, u32 pc) const {
    if (opOf(insn) != Op::syscall) {
        return std::nullopt;
    }
    return Trap{std::format("if (!hostCall(m, {}, 0x{:08x}u)) return false;",
                            importIndex(insn.word), pc),
                false};
}

}  // namespace

std::string translateIop(Memory& memory, const Program& program) {
    return IopTranslator(memory, program).run();
}

}  // namespace uee
