#include "iop_kernel.h"
#include "iop_translate.h"

#include "irx.h"
#include "translator.h"

#include <cctype>
#include <stdexcept>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uee {
namespace {

// The name is a namespace, a symbol and what the loader matches on.
void requireIdentifier(std::string_view name) {
    const auto usable = [](char letter, bool first) {
        return letter == '_' || std::isalpha(static_cast<unsigned char>(letter)) != 0 ||
               (!first && std::isdigit(static_cast<unsigned char>(letter)) != 0);
    };
    for (usize i = 0; i < name.size(); ++i) {
        if (!usable(name[i], i == 0)) {
            throw std::runtime_error(std::format(
                "the module name \"{}\" cannot be a C++ identifier, which is what the "
                "translated module is named after", name));
        }
    }
    if (name.empty()) {
        throw std::runtime_error("a translated module needs a name");
    }
}

class IopTranslator final : public Translator {
public:
    IopTranslator(Memory& memory, const Program& program, std::string_view name,
                  std::span<const Import> imports, const Symbols* symbols,
                  std::string_view loadName = {}, std::span<const u32> entries = {},
                  const std::set<std::string>& guestLibraries = {})
        : Translator(memory, program, symbols, entries), mGuestLibraries(guestLibraries),
          mName(name),
          mLoadName(loadName.empty() ? name : loadName), mBase(program.codeStart),
          mEnd(program.imageEnd), mImports(imports) {
        requireIdentifier(mName);
    }

private:
    LoadDelay getLoadDelay() const override { return LoadDelay::OneInstruction; }
    std::optional<std::string> emitInlinedCall(u32 target, u32 returnTo) const override {
        const std::optional<u32> word = mMemory.peek32(target);
        if (!word || getOp(Instruction{*word}) != Op::syscall ||
            mCode.getFunctionEnd(target) != target + 4) {
            return {};
        }
        // The stub committed the caller's pending load on entry, and the call
        // reads $ra expecting that to have happened.
        const u32 index = importIndex(*word);
        if (const std::optional<u32> service = getHostService(index)) {
            return std::format("commitLoad(iop.cpu);\n    "
                               "hostService(m, {}, 0x{:08x}u);\n    "
                               "if (iop.cpu.pc != 0x{:08x}u) return false;",
                               *service, target, returnTo);
        }
        return std::format("commitLoad(iop.cpu);\n    "
                           "if (!hostCall(m, {}Module.importBase + {}, 0x{:08x}u) || "
                           "iop.cpu.pc != 0x{:08x}u) return false;",
                           mName, index, target, returnTo);
    }

    std::string_view getMachineType() const override { return "IopMachine"; }
    std::string_view getContextType() const override { return "IopContext"; }
    std::string_view getStateName() const override { return "iop"; }
    std::string_view getHandlersHeader() const override { return "iop_handlers.h"; }
    std::string_view getRuntimeHeader() const override { return "iop_translate.h"; }
    std::string emitBeforeInstruction() const override { return "commitLoad(iop.cpu);"; }

    std::string readGpr(u32 reg) const override;
    std::string writeGpr(u32 reg, u32 value) const override;
    std::string writeConstant(u32 reg, u32 value) const override;
    std::optional<std::string> emitOne(u32 pc, Instruction insn) const override;
    std::optional<std::string> emitBranchCondition(Op op, Instruction insn) const override;
    std::optional<Trap> emitTrap(Instruction insn, const BlockValues& values,
                                 u32 pc) const override;
    std::string emitNamespaceOpen() const override;
    std::string emitNamespaceClose() const override;
    std::string emitBeforeNamespace() const override;
    std::string emitDefinitions() const override;

    // Only where the host is the only answer: a library a guest module exports
    // binds there first, as the IOP's own linker does.
    std::optional<u32> getHostService(u32 index) const {
        if (index >= mImports.size() ||
            mGuestLibraries.count(std::string(mImports[index].module)) != 0) {
            return std::nullopt;
        }
        return IopKernel::findService(mImports[index].module, mImports[index].id);
    }

    // A library a translated module provides is one the guest answers, so the
    // host service of the same name is not the one a call reaches.
    std::set<std::string> mGuestLibraries;
    std::string mName;
    std::string mLoadName;
    u32 mBase;
    u32 mEnd;
    std::span<const Import> mImports;
};

std::string IopTranslator::emitBeforeNamespace() const {
    return std::format("\nextern TranslatedIrx {}Module;\n", mName);
}

std::string IopTranslator::emitNamespaceOpen() const {
    return std::format("\nnamespace translated {{\nnamespace {} {{\n", mName);
}

std::string IopTranslator::emitNamespaceClose() const {
    return std::format("\n}}  // namespace {}\n}}  // namespace translated\n", mName);
}

// Named, not indexed: the index depends on where the loader puts this block.
std::string IopTranslator::emitDefinitions() const {
    std::string out = "\nconstexpr TranslatedImport kImports[] = {\n";
    for (const Import& import : mImports) {
        out += std::format("    {{\"{}\", {}}},\n", import.module, import.id);
    }
    out += "};\n";
    out += std::format("\n}}  // namespace {0}\n}}  // namespace translated\n"
                       "\nTranslatedIrx {0}Module{{\n"
                       "    \"{3}\", 0x{1:08x}u, 0x{2:08x}u, translated::{0}::kImports,\n"
                       "    translated::{0}::runFunction}};\n"
                       "\nnamespace translated {{\nnamespace {0} {{\n",
                       mName, mBase, mEnd, mLoadName);
    return out;
}

std::optional<std::string> IopTranslator::emitOne(u32 pc, Instruction insn) const {
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
        return std::format("{}(iop.cpu, mem, {}, {}, {});", name, r(a), emitBase(pc, base),
                           offset);
    };

    switch (getOp(insn)) {
        case Op::sll: return shift("sll", rd, rt, sa);
        case Op::srl: return shift("srl", rd, rt, sa);
        case Op::sra: return shift("sra", rd, rt, sa);
        case Op::sllv: return gpr3("sllv", rd, rt, rs);
        case Op::srlv: return gpr3("srlv", rd, rt, rs);
        case Op::srav: return gpr3("srav", rd, rt, rs);

        case Op::add:
        case Op::addu:
            if (rt == 0) return gpr2("move", rd, rs);
            if (rs == 0) return gpr2("move", rd, rt);
            return gpr3("addu", rd, rs, rt);
        case Op::sub:
        case Op::subu: return gpr3("subu", rd, rs, rt);
        case Op::and_: return gpr3("and_", rd, rs, rt);
        case Op::or_:
            if (rt == 0) return gpr2("move", rd, rs);
            if (rs == 0) return gpr2("move", rd, rt);
            return gpr3("or_", rd, rs, rt);
        case Op::xor_: return gpr3("xor_", rd, rs, rt);
        case Op::nor: return gpr3("nor", rd, rs, rt);
        case Op::slt: return gpr3("slt", rd, rs, rt);
        case Op::sltu: return gpr3("sltu", rd, rs, rt);

        case Op::addi:
        case Op::addiu:
            return simm == 0 ? gpr2("move", rt, rs) : withImm("addiu", rt, rs, simm);
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
    return writeConstant(reg, value);
}

std::string IopTranslator::writeConstant(u32 reg, u32 value) const {
    return std::format("iop.cpu.write({}, 0x{:08x}u);", regName(reg), value);
}

// The only syscalls a module carries are import stubs, which resolve the pc
// themselves.
std::optional<Trap> IopTranslator::emitTrap(Instruction insn, const BlockValues&,
                                            u32 pc) const {
    if (getOp(insn) == Op::break_) {
        return Trap{std::format("raiseException(iop.cpu, 0x{:08x}u, r3000::kBreakpoint);", pc),
                    false};
    }
    if (getOp(insn) != Op::syscall) {
        return std::nullopt;
    }
    const u32 index = importIndex(insn.word);
    if (const std::optional<u32> service = getHostService(index)) {
        return Trap{std::format("hostService(m, {}, 0x{:08x}u);", *service, pc), false};
    }
    return Trap{std::format("if (!hostCall(m, {}Module.importBase + {}, 0x{:08x}u)) return false;",
                            mName, index, pc),
                false};
}

}  // namespace

TranslatedSource translateIop(Memory& memory, const Program& program, std::string_view name,
                              std::span<const Import> imports, usize units,
                              std::string_view declarationsHeader, std::string_view loadName,
                              std::span<const u32> entries,
                              const std::set<std::string>& guestLibraries) {
    return IopTranslator(memory, program, name, imports, nullptr, loadName, entries,
                         guestLibraries)
        .run(units, declarationsHeader);
}

}  // namespace uee
