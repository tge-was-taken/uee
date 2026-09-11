#include "syscall_table.h"
#include "ee_translate.h"

#include "handlers.h"
#include "instruction.h"
#include "translator.h"

#include <format>
#include <optional>
#include <string>

namespace uee {

std::string_view regName(u32 index) {
    static constexpr std::string_view names[32] = {
        "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2",
        "t3",   "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5",
        "s6",   "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};
    return names[index & 31];
}

namespace {

// Coprocessor and control registers index a different file, so they stay numeric.
constexpr u32 kSyscallNumberRegister = 3;

// The syscall number arrives in $v1 and every compiler loads it with a single
// immediate right before the call, so only that shape is recognised.
std::optional<s32> syscallNumberBefore(Instruction insn) {
    const Op op = getOp(insn);
    if (insn.rt() != kSyscallNumberRegister || insn.rs() != 0) {
        return std::nullopt;
    }
    if (op == Op::addiu || op == Op::daddiu) {
        return static_cast<s32>(insn.simm());
    }
    if (op == Op::ori) {
        return static_cast<s32>(insn.imm());
    }
    return std::nullopt;
}

// The widths whose main-memory form is a plain load or store. The unaligned
// family reads the address twice over and keeps its general form.
bool reachesRam(Op op) {
    switch (op) {
        case Op::lb: case Op::lbu: case Op::lh: case Op::lhu:
        case Op::lw: case Op::lwu: case Op::ld: case Op::lq:
        case Op::sb: case Op::sh: case Op::sw: case Op::sd: case Op::sq:
            return true;
        default:
            return false;
    }
}

constexpr u32 kVif1Chcr = 0x10009000;
constexpr u32 kGifChcr = 0x1000A000;

std::string_view blockName(EeMmioBlock block) {
    switch (block) {
        case EeMmioBlock::Timers: return "Timers";
        case EeMmioBlock::Dmac: return "Dmac";
        case EeMmioBlock::Interrupts: return "Interrupts";
        case EeMmioBlock::Sbus: return "Sbus";
        case EeMmioBlock::Gif: return "Gif";
        case EeMmioBlock::Gs: return "Gs";
        case EeMmioBlock::Ipu: return "Ipu";
        case EeMmioBlock::Vif0: return "Vif0";
        case EeMmioBlock::Vif1: return "Vif1";
        case EeMmioBlock::None: return "None";
    }
    return "None";
}

// A register window is only reached by the widths the runtime decodes there;
// a narrower access to one goes to the backing store either way.
bool reachesRegisters(Op op) {
    switch (op) {
        case Op::lw: case Op::sw: case Op::ld: case Op::sd:
            return true;
        default:
            return false;
    }
}

std::string_view vuHandlerName(VuKind kind, bool toAcc) {
    switch (kind) {
        case VuKind::Add: return toAcc ? "vadda" : "vadd";
        case VuKind::Sub: return toAcc ? "vsuba" : "vsub";
        case VuKind::Mul: return toAcc ? "vmula" : "vmul";
        case VuKind::Max: return "vmax";
        case VuKind::Mini: return "vmini";
        case VuKind::Madd: return toAcc ? "vmadda" : "vmadd";
        case VuKind::Msub: return toAcc ? "vmsuba" : "vmsub";
    }
    return {};
}

std::string_view vuSourceName(VuSource source) {
    switch (source) {
        case VuSource::X: return "X";
        case VuSource::Y: return "Y";
        case VuSource::Z: return "Z";
        case VuSource::W: return "W";
        case VuSource::Q: return "Q";
        case VuSource::I: return "I";
        default: return "Vt";
    }
}

class EeTranslator final : public Translator {
public:
    using Translator::Translator;

private:
    LoadDelay getLoadDelay() const override { return LoadDelay::None; }
    std::string emitHookCheck(u32 function) const override {
        if (mHookable.count(function) == 0) {
            return {};
        }
        return std::format("    if (enterHook(m, 0x{:08x}u)) {{\n"
                           "        return leave(ee, ee.cpu.pc, returnTo);\n    }}\n",
                           function);
    }

    std::string_view getMachineType() const override { return "Machine"; }
    std::string_view getContextType() const override { return "EeContext"; }
    std::string_view getStateName() const override { return "ee"; }
    std::string_view getHandlersHeader() const override { return "handlers.h"; }
    std::string_view getRuntimeHeader() const override { return "ee_translate.h"; }

    std::string readGpr(u32 reg) const override;
    std::string writeGpr(u32 reg, u32 value) const override;
    std::string writeConstant(u32 reg, u32 value) const override;
    std::optional<std::string> emitOne(u32 pc, Instruction insn) const override;
    std::optional<std::string> emitOperation(u32 pc, Instruction insn) const;
    bool followsVectorUnitOp(u32 pc) const;
    std::optional<std::string> emitBranchCondition(Op op, Instruction insn) const override;
    std::optional<Trap> emitTrap(Instruction insn, const BlockValues& values,
                                 u32 pc) const override;
    std::string countInstructions(u32 count, u32 blockStart) const override;
    std::string countInstructionsOnly(u32 count) const override;
};

// Every macro instruction the vector unit executes, which is every one whose
// name starts with a v, moves the quotient register a step along.
bool isVectorUnitOp(Op op) {
    const std::string_view name = lookupName(op);
    return !name.empty() && name.front() == 'v';
}

// The flag registers are two deep, so only an instruction within two of the
// one that filled them can be the point where the handover shows.
bool EeTranslator::followsVectorUnitOp(u32 pc) const {
    for (u32 back = 4; back <= 8; back += 4) {
        const std::optional<u32> word = mMemory.peek32(pc - back);
        if (word && isVectorUnitOp(getOp(Instruction{*word}))) {
            return true;
        }
    }
    return false;
}

std::optional<std::string> EeTranslator::emitOne(u32 pc, Instruction insn) const {
    const std::optional<std::string> body = emitOperation(pc, insn);
    if (!body || !isVectorUnitOp(getOp(insn))) {
        return body;
    }
    return "ee.vu0.beginMacro(); " + *body + " ee.vu0.endMacro();";
}

std::optional<std::string> EeTranslator::emitOperation(u32 pc, Instruction insn) const {
    const Op op = getOp(insn);
    const u32 rs = insn.rs();
    const u32 rt = insn.rt();
    const u32 rd = insn.rd();
    const u32 sa = insn.sa();
    const u32 imm = insn.imm();
    const s32 simm = insn.simm();
    const auto r = [](u32 i) { return regName(i); };

    // sll zero, zero, 0 is the canonical nop.
    if (insn.word == 0) {
        return followsVectorUnitOp(pc) ? std::string{"ee.vu0.advanceFlags();"}
                                       : std::string{};
    }

    const auto gpr3 = [&](const char* name, u32 a, u32 b, u32 c) {
        return std::format("{}(ee.cpu, {}, {}, {});", name, r(a), r(b), r(c));
    };
    const auto gpr2 = [&](const char* name, u32 a, u32 b) {
        return std::format("{}(ee.cpu, {}, {});", name, r(a), r(b));
    };
    const auto gpr1 = [&](const char* name, u32 a) {
        return std::format("{}(ee.cpu, {});", name, r(a));
    };
    const auto shift = [&](const char* name, u32 a, u32 b, u32 amount) {
        return std::format("{}(ee.cpu, {}, {}, {});", name, r(a), r(b), amount);
    };
    const auto withImm = [&](const char* name, u32 a, u32 b, auto value) {
        return std::format("{}(ee.cpu, {}, {}, {});", name, r(a), r(b), value);
    };
    // A resolved address needs neither the register read nor the search that
    // decides which window it lands in.
    const auto access = [&](const char* name, u32 a, u32 base, s32 offset) {
        const std::optional<Resolved> at = resolveAccess(pc, base, offset);
        if (at && at->window == Window::Ram && reachesRam(op)) {
            return std::format("{}Ram(ee.cpu, mem, {}, 0x{:08x}u);", name, r(a), at->address);
        }
        if (at && at->window == Window::Mmio && reachesRegisters(op)) {
            // Writing a channel's control register is what starts a transfer.
            if (at->address == kVif1Chcr || at->address == kGifChcr) {
                noteRenderKick(pc);
            }
            return std::format("{}Mmio<EeMmioBlock::{}>(ee.cpu, m.mmio, mem, {}, 0x{:08x}u);", name,
                               blockName(mmioBlockOf(at->address)), r(a), at->address);
        }
        return std::format("{}(ee.cpu, mem, {}, {}, {});", name, r(a), emitBase(pc, base),
                           offset);
    };
    const auto pipe = [&](const char* name, u32 a, u32 b, u32 c, bool one) {
        return std::format("{}(ee.cpu, {}, {}, {}, {});", name, r(a), r(b), r(c), one);
    };
    const auto halfPipe = [&](const char* name, u32 a, bool one) {
        return std::format("{}(ee.cpu, {}, {});", name, r(a), one);
    };
    const auto fpu3 = [&](const char* name, u32 a, u32 b, u32 c) {
        return std::format("{}(ee.fpu, {}, {}, {});", name, a, b, c);
    };
    const auto fpu2 = [&](const char* name, u32 a, u32 b) {
        return std::format("{}(ee.fpu, {}, {});", name, a, b);
    };
    const auto vu2 = [&](const char* name, u32 a, u32 b) {
        return std::format("{}(ee.vu0, {}, {});", name, a, b);
    };
    const auto vu3 = [&](const char* name, u32 a, u32 b, u32 c) {
        return std::format("{}(ee.vu0, {}, {}, {});", name, a, b, c);
    };
    // The three that fill the quotient register hand it over late.
    const auto delayQuotient = [](const std::string& call) {
        return std::format("{{ const u32 previous = ee.vu0.vi[Vu::kQ]; {} "
                           "ee.vu0.delayQuotient(previous); }}",
                           call);
    };
    const auto vuField = [&](const char* name, u32 field, u32 a, u32 b) {
        return std::format("{}(ee.vu0, 0x{:x}u, {}, {});", name, field, a, b);
    };
    const auto vuFixed = [&](const char* name, u32 field, u32 a, u32 b, s32 fraction) {
        return std::format("{}(ee.vu0, 0x{:x}u, {}, {}, {});", name, field, a, b, fraction);
    };
    const auto vuAccess = [&](const char* name, u32 field, u32 a, u32 b) {
        return std::format("{}(ee.vu0, mem, 0x{:x}u, {}, {});", name, field, a, b);
    };
    // Macro mode reaches the first unit's memory, whichever unit the same
    // instruction addresses when a microprogram runs it.
    const auto vuWindowAccess = [&](const char* name, u32 field, u32 a, u32 b) {
        return std::format("{}(ee.vu0, mem, 0x{:x}u, {}, {}, kVu0DataBase, kVu0MemorySize);",
                           name, field, a, b);
    };
    // Nothing here handles the exception a taken trap raises, so the driver is
    // handed the instruction to report. Not taking one costs a comparison.
    const auto trap = [&](const char* name, const std::string& operands) {
        return std::format("if ({}(ee.cpu, {})) {{ interpreterFallback(m, 0x{:08x}u); "
                           "return leave(ee, ee.cpu.pc, returnTo); }}", name, operands, pc);
    };
    const auto trapPair = [&](const char* name) {
        return trap(name, std::format("{}, {}", r(rs), r(rt)));
    };
    const auto trapImmediate = [&](const char* name) {
        return trap(name, std::format("{}, {}", r(rs), simm));
    };

    switch (op) {
        case Op::sll: return shift("sll", rd, rt, sa);
        case Op::srl: return shift("srl", rd, rt, sa);
        case Op::sra: return shift("sra", rd, rt, sa);
        case Op::dsll: return shift("dsll", rd, rt, sa);
        case Op::dsrl: return shift("dsrl", rd, rt, sa);
        case Op::dsra: return shift("dsra", rd, rt, sa);
        case Op::dsll32: return shift("dsll32", rd, rt, sa);
        case Op::dsrl32: return shift("dsrl32", rd, rt, sa);
        case Op::dsra32: return shift("dsra32", rd, rt, sa);

        case Op::sllv: return gpr3("sllv", rd, rt, rs);
        case Op::srlv: return gpr3("srlv", rd, rt, rs);
        case Op::srav: return gpr3("srav", rd, rt, rs);
        case Op::dsllv: return gpr3("dsllv", rd, rt, rs);
        case Op::dsrlv: return gpr3("dsrlv", rd, rt, rs);
        case Op::dsrav: return gpr3("dsrav", rd, rt, rs);

        case Op::add:
        case Op::addu:
            if (rt == 0) return gpr2("moveWord", rd, rs);
            if (rs == 0) return gpr2("moveWord", rd, rt);
            return gpr3("addu", rd, rs, rt);
        case Op::sub:
        case Op::subu: return gpr3("subu", rd, rs, rt);
        case Op::dadd:
        case Op::daddu:
            if (rt == 0) return gpr2("move", rd, rs);
            if (rs == 0) return gpr2("move", rd, rt);
            return gpr3("daddu", rd, rs, rt);
        case Op::dsub:
        case Op::dsubu: return gpr3("dsubu", rd, rs, rt);
        case Op::and_: return gpr3("and_", rd, rs, rt);
        case Op::or_:
            if (rt == 0) return gpr2("move", rd, rs);
            if (rs == 0) return gpr2("move", rd, rt);
            return gpr3("or_", rd, rs, rt);
        case Op::xor_: return gpr3("xor_", rd, rs, rt);
        case Op::nor: return gpr3("nor", rd, rs, rt);
        case Op::slt: return gpr3("slt", rd, rs, rt);
        case Op::sltu: return gpr3("sltu", rd, rs, rt);
        case Op::movz: return gpr3("movz", rd, rs, rt);
        case Op::movn: return gpr3("movn", rd, rs, rt);

        case Op::addi:
        case Op::addiu:
            return simm == 0 ? gpr2("moveWord", rt, rs) : withImm("addiu", rt, rs, simm);
        case Op::daddi:
        case Op::daddiu:
            return simm == 0 ? gpr2("move", rt, rs) : withImm("daddiu", rt, rs, simm);
        case Op::andi: return withImm("andi", rt, rs, imm);
        case Op::ori: return withImm("ori", rt, rs, imm);
        case Op::xori: return withImm("xori", rt, rs, imm);
        case Op::slti: return withImm("slti", rt, rs, simm);
        case Op::sltiu: return withImm("sltiu", rt, rs, simm);
        case Op::lui: return std::format("lui(ee.cpu, {}, {});", r(rt), imm);

        case Op::pand: return gpr3("pand", rd, rs, rt);
        case Op::por: return gpr3("por", rd, rs, rt);
        case Op::pxor: return gpr3("pxor", rd, rs, rt);
        case Op::pnor: return gpr3("pnor", rd, rs, rt);
        case Op::pcpyld: return gpr3("pcpyld", rd, rs, rt);
        case Op::pcpyud: return gpr3("pcpyud", rd, rs, rt);
        case Op::pextlw: return gpr3("pextlw", rd, rs, rt);
        case Op::pextuw: return gpr3("pextuw", rd, rs, rt);
        case Op::paddw: return gpr3("paddw", rd, rs, rt);
        case Op::psubw: return gpr3("psubw", rd, rs, rt);
        case Op::psubb: return gpr3("psubb", rd, rs, rt);
        case Op::paddub: return gpr3("paddub", rd, rs, rt);
        case Op::padduw: return gpr3("padduw", rd, rs, rt);
        case Op::pextlb: return gpr3("pextlb", rd, rs, rt);
        case Op::pextlh: return gpr3("pextlh", rd, rs, rt);
        case Op::ppacb: return gpr3("ppacb", rd, rs, rt);
        case Op::ppach: return gpr3("ppach", rd, rs, rt);
        case Op::ppacw: return gpr3("ppacw", rd, rs, rt);
        case Op::pexew: return gpr2("pexew", rd, rt);
        case Op::prot3w: return gpr2("prot3w", rd, rt);
        case Op::pcpyh: return gpr2("pcpyh", rd, rt);
        case Op::plzcw: return gpr2("plzcw", rd, rs);

        case Op::mult: return pipe("mult", rd, rs, rt, false);
        case Op::multu: return pipe("multu", rd, rs, rt, false);
        case Op::mult1: return pipe("mult", rd, rs, rt, true);
        case Op::multu1: return pipe("multu", rd, rs, rt, true);
        case Op::madd: return pipe("madd", rd, rs, rt, false);

        case Op::paddb: return gpr3("paddb", rd, rs, rt);
        case Op::paddh: return gpr3("paddh", rd, rs, rt);
        case Op::psubh: return gpr3("psubh", rd, rs, rt);
        case Op::paddsb: return gpr3("paddsb", rd, rs, rt);
        case Op::paddsh: return gpr3("paddsh", rd, rs, rt);
        case Op::paddsw: return gpr3("paddsw", rd, rs, rt);
        case Op::psubsb: return gpr3("psubsb", rd, rs, rt);
        case Op::psubsh: return gpr3("psubsh", rd, rs, rt);
        case Op::psubsw: return gpr3("psubsw", rd, rs, rt);
        case Op::padduh: return gpr3("padduh", rd, rs, rt);
        case Op::psubub: return gpr3("psubub", rd, rs, rt);
        case Op::psubuh: return gpr3("psubuh", rd, rs, rt);
        case Op::psubuw: return gpr3("psubuw", rd, rs, rt);
        case Op::pmaxh: return gpr3("pmaxh", rd, rs, rt);
        case Op::pminh: return gpr3("pminh", rd, rs, rt);
        case Op::pmaxw: return gpr3("pmaxw", rd, rs, rt);
        case Op::pminw: return gpr3("pminw", rd, rs, rt);
        case Op::pceqb: return gpr3("pceqb", rd, rs, rt);
        case Op::pceqh: return gpr3("pceqh", rd, rs, rt);
        case Op::pceqw: return gpr3("pceqw", rd, rs, rt);
        case Op::pcgtb: return gpr3("pcgtb", rd, rs, rt);
        case Op::pcgth: return gpr3("pcgth", rd, rs, rt);
        case Op::pcgtw: return gpr3("pcgtw", rd, rs, rt);
        case Op::pextub: return gpr3("pextub", rd, rs, rt);
        case Op::pextuh: return gpr3("pextuh", rd, rs, rt);
        case Op::pinth: return gpr3("pinth", rd, rs, rt);
        case Op::pinteh: return gpr3("pinteh", rd, rs, rt);
        case Op::padsbh: return gpr3("padsbh", rd, rs, rt);
        case Op::qfsrv: return gpr3("qfsrv", rd, rs, rt);
        case Op::pmultw: return gpr3("pmultw", rd, rs, rt);
        case Op::pmultuw: return gpr3("pmultuw", rd, rs, rt);
        case Op::pmaddw: return gpr3("pmaddw", rd, rs, rt);
        case Op::pmadduw: return gpr3("pmadduw", rd, rs, rt);
        case Op::pmsubw: return gpr3("pmsubw", rd, rs, rt);
        case Op::pmulth: return gpr3("pmulth", rd, rs, rt);
        case Op::pmaddh: return gpr3("pmaddh", rd, rs, rt);
        case Op::pmsubh: return gpr3("pmsubh", rd, rs, rt);
        case Op::phmadh: return gpr3("phmadh", rd, rs, rt);
        case Op::phmsbh: return gpr3("phmsbh", rd, rs, rt);
        case Op::pabsh: return gpr2("pabsh", rd, rt);
        case Op::pabsw: return gpr2("pabsw", rd, rt);
        case Op::prevh: return gpr2("prevh", rd, rt);
        case Op::pexeh: return gpr2("pexeh", rd, rt);
        case Op::pexch: return gpr2("pexch", rd, rt);
        case Op::pexcw: return gpr2("pexcw", rd, rt);
        case Op::pext5: return gpr2("pext5", rd, rt);
        case Op::ppac5: return gpr2("ppac5", rd, rt);
        case Op::psllh: return std::format("psllh(ee.cpu, {}, {}, {});", r(rd), r(rt), sa);
        case Op::psrlh: return std::format("psrlh(ee.cpu, {}, {}, {});", r(rd), r(rt), sa);
        case Op::psrah: return std::format("psrah(ee.cpu, {}, {}, {});", r(rd), r(rt), sa);
        case Op::psllw: return std::format("psllw(ee.cpu, {}, {}, {});", r(rd), r(rt), sa);
        case Op::psrlw: return std::format("psrlw(ee.cpu, {}, {}, {});", r(rd), r(rt), sa);
        case Op::psraw: return std::format("psraw(ee.cpu, {}, {}, {});", r(rd), r(rt), sa);
        case Op::psllvw: return gpr3("psllvw", rd, rt, rs);
        case Op::psrlvw: return gpr3("psrlvw", rd, rt, rs);
        case Op::psravw: return gpr3("psravw", rd, rt, rs);
        case Op::pdivw: return std::format("pdivw(ee.cpu, {}, {});", r(rs), r(rt));
        case Op::pdivuw: return std::format("pdivuw(ee.cpu, {}, {});", r(rs), r(rt));
        case Op::pdivbw: return std::format("pdivbw(ee.cpu, {}, {});", r(rs), r(rt));
        case Op::pmfhl_uw: return std::format("pmfhl_uw(ee.cpu, {});", r(rd));
        case Op::pmfhl_slw: return std::format("pmfhl_slw(ee.cpu, {});", r(rd));
        case Op::pmfhl_lh: return std::format("pmfhl_lh(ee.cpu, {});", r(rd));
        case Op::pmfhl_sh: return std::format("pmfhl_sh(ee.cpu, {});", r(rd));
        case Op::pmthl_lw: return std::format("pmthl_lw(ee.cpu, {});", r(rs));
        case Op::madd1: return pipe("madd", rd, rs, rt, true);
        case Op::maddu: return pipe("maddu", rd, rs, rt, false);
        case Op::maddu1: return pipe("maddu", rd, rs, rt, true);
        case Op::div: return std::format("div(ee.cpu, {}, {}, false);", r(rs), r(rt));
        case Op::divu: return std::format("divu(ee.cpu, {}, {}, false);", r(rs), r(rt));
        case Op::div1: return std::format("div(ee.cpu, {}, {}, true);", r(rs), r(rt));
        case Op::divu1: return std::format("divu(ee.cpu, {}, {}, true);", r(rs), r(rt));

        case Op::mthi: return halfPipe("mthi", rs, false);
        case Op::mtlo: return halfPipe("mtlo", rs, false);
        case Op::mthi1: return halfPipe("mthi", rs, true);
        case Op::mtlo1: return halfPipe("mtlo", rs, true);
        case Op::mfhi: return halfPipe("mfhi", rd, false);
        case Op::mflo: return halfPipe("mflo", rd, false);
        case Op::mfhi1: return halfPipe("mfhi", rd, true);
        case Op::mflo1: return halfPipe("mflo", rd, true);
        case Op::pmfhi: return gpr1("pmfhi", rd);
        case Op::pmflo: return gpr1("pmflo", rd);
        case Op::pmfhl_lw: return gpr1("pmfhl_lw", rd);
        case Op::pmthi: return gpr1("pmthi", rs);
        case Op::pmtlo: return gpr1("pmtlo", rs);

        case Op::mfsa: return gpr1("mfsa", rd);
        case Op::mtsa: return gpr1("mtsa", rs);
        case Op::mtsab: return std::format("mtsab(ee.cpu, {}, {});", r(rs), imm);
        case Op::mtsah: return std::format("mtsah(ee.cpu, {}, {});", r(rs), imm);

        case Op::mfc0:
            return std::format("mfc0(ee.cpu, {}, {}, {});", r(rt), rd, insn.word & 0x3Fu);
        case Op::mtc0:
            return std::format("mtc0(ee.cpu, {}, {}, {});", r(rt), rd, insn.word & 0x3Fu);
        case Op::di: return "di(ee.cpu);";
        case Op::ei: return "ei(ee.cpu);";

        case Op::mfc1: return std::format("mfc1(ee.cpu, ee.fpu, {}, {});", r(rt), insn.fs());
        case Op::mtc1:
            // A float constant is built in a general register and moved across,
            // so the move is where the constant becomes the value.
            if (const std::optional<u32> value = getKnownValue(pc, rt)) {
                return std::format("ee.fpu.fpr[{}] = 0x{:08x}u;", insn.fs(), *value);
            }
            return std::format("mtc1(ee.cpu, ee.fpu, {}, {});", r(rt), insn.fs());
        case Op::cfc1:
            return std::format("cfc1(ee.cpu, ee.fpu, {}, {});", r(rt), insn.fs());
        case Op::ctc1:
            return std::format("ctc1(ee.cpu, ee.fpu, {}, {});", r(rt), insn.fs());

        case Op::qmfc2_ni:
            return std::format("qmfc2(ee.cpu, ee.vu0, {}, {}, {});", r(rt), insn.vs(),
                               (insn.word & 1u) != 0 ? "true" : "false");
        case Op::qmtc2_ni:
            return std::format("qmtc2(ee.cpu, ee.vu0, {}, {});", r(rt), insn.vs());
        case Op::cfc2_ni:
            return std::format("cfc2(ee.cpu, ee.vu0, {}, {}, {});", r(rt), rd,
                               (insn.word & 1u) != 0 ? "true" : "false");
        case Op::ctc2_ni:
            return std::format("ctc2(ee.cpu, ee, mem, {}, {});", r(rt), rd);

        case Op::lb: return access("lb", rt, rs, simm);
        case Op::lbu: return access("lbu", rt, rs, simm);
        case Op::lh: return access("lh", rt, rs, simm);
        case Op::lhu: return access("lhu", rt, rs, simm);
        case Op::lw: return access("lw", rt, rs, simm);
        case Op::lwu: return access("lwu", rt, rs, simm);
        case Op::ld: return access("ld", rt, rs, simm);
        case Op::lq: return access("lq", rt, rs, simm);
        case Op::sb: return access("sb", rt, rs, simm);
        case Op::sh: return access("sh", rt, rs, simm);
        case Op::sw: return access("sw", rt, rs, simm);
        case Op::sd: return access("sd", rt, rs, simm);
        case Op::sq: return access("sq", rt, rs, simm);
        case Op::lwl: return access("lwl", rt, rs, simm);
        case Op::lwr: return access("lwr", rt, rs, simm);
        case Op::ldl: return access("ldl", rt, rs, simm);
        case Op::ldr: return access("ldr", rt, rs, simm);
        case Op::swl: return access("swl", rt, rs, simm);
        case Op::swr: return access("swr", rt, rs, simm);
        case Op::sdl: return access("sdl", rt, rs, simm);
        case Op::sdr: return access("sdr", rt, rs, simm);

        case Op::lwc1:
            return std::format("lwc1(ee.cpu, ee.fpu, mem, {}, {}, {});", rt, emitBase(pc, rs), simm);
        case Op::swc1:
            return std::format("swc1(ee.cpu, ee.fpu, mem, {}, {}, {});", rt, emitBase(pc, rs), simm);
        case Op::lqc2:
            return std::format("lqc2(ee.cpu, ee.vu0, mem, {}, {}, {});", rt, emitBase(pc, rs), simm);
        case Op::sqc2:
            return std::format("sqc2(ee.cpu, ee.vu0, mem, {}, {}, {});", rt, emitBase(pc, rs), simm);

        case Op::add_s: return fpu3("add_s", insn.fd(), insn.fs(), insn.ft());
        case Op::sub_s: return fpu3("sub_s", insn.fd(), insn.fs(), insn.ft());
        case Op::mul_s: return fpu3("mul_s", insn.fd(), insn.fs(), insn.ft());
        case Op::div_s: return fpu3("div_s", insn.fd(), insn.fs(), insn.ft());
        case Op::madd_s: return fpu3("madd_s", insn.fd(), insn.fs(), insn.ft());
        case Op::msub_s: return fpu3("msub_s", insn.fd(), insn.fs(), insn.ft());
        case Op::max_s: return fpu3("max_s", insn.fd(), insn.fs(), insn.ft());
        case Op::min_s: return fpu3("min_s", insn.fd(), insn.fs(), insn.ft());
        case Op::rsqrt_s: return fpu3("rsqrt_s", insn.fd(), insn.fs(), insn.ft());
        case Op::adda_s: return fpu2("adda_s", insn.fs(), insn.ft());
        case Op::suba_s: return fpu2("suba_s", insn.fs(), insn.ft());
        case Op::mula_s: return fpu2("mula_s", insn.fs(), insn.ft());
        case Op::madda_s: return fpu2("madda_s", insn.fs(), insn.ft());
        case Op::msuba_s: return fpu2("msuba_s", insn.fs(), insn.ft());
        case Op::sqrt_s: return fpu2("sqrt_s", insn.fd(), insn.ft());
        case Op::abs_s: return fpu2("abs_s", insn.fd(), insn.fs());
        case Op::neg_s: return fpu2("neg_s", insn.fd(), insn.fs());
        case Op::mov_s: return fpu2("mov_s", insn.fd(), insn.fs());
        case Op::cvt_w_s: return fpu2("cvt_w_s", insn.fd(), insn.fs());
        case Op::cvt_s_w: return fpu2("cvt_s_w", insn.fd(), insn.fs());
        case Op::c_f_s: return std::string{"c_f_s(ee.fpu);"};
        case Op::c_eq_s: return fpu2("c_eq_s", insn.fs(), insn.ft());
        case Op::c_lt_s: return fpu2("c_lt_s", insn.fs(), insn.ft());
        case Op::c_le_s: return fpu2("c_le_s", insn.fs(), insn.ft());

        case Op::vmove: return vuField("vmove", insn.dest(), insn.vt(), insn.vs());
        case Op::vmr32: return vuField("vmr32", insn.dest(), insn.vt(), insn.vs());
        case Op::vabs: return vuField("vabs", insn.dest(), insn.vt(), insn.vs());
        case Op::vitof0: return vuFixed("vitof", insn.dest(), insn.vt(), insn.vs(), 0);
        case Op::vitof4: return vuFixed("vitof", insn.dest(), insn.vt(), insn.vs(), 4);
        case Op::vitof12: return vuFixed("vitof", insn.dest(), insn.vt(), insn.vs(), 12);
        case Op::vitof15: return vuFixed("vitof", insn.dest(), insn.vt(), insn.vs(), 15);
        case Op::vftoi0: return vuFixed("vftoi", insn.dest(), insn.vt(), insn.vs(), 0);
        case Op::vftoi4: return vuFixed("vftoi", insn.dest(), insn.vt(), insn.vs(), 4);
        case Op::vftoi12: return vuFixed("vftoi", insn.dest(), insn.vt(), insn.vs(), 12);
        case Op::vftoi15: return vuFixed("vftoi", insn.dest(), insn.vt(), insn.vs(), 15);
        case Op::viadd: return vu3("viadd", insn.vid(), insn.vis(), insn.vit());
        case Op::visub: return vu3("visub", insn.vid(), insn.vis(), insn.vit());
        case Op::viand: return vu3("viand", insn.vid(), insn.vis(), insn.vit());
        case Op::vior: return vu3("vior", insn.vid(), insn.vis(), insn.vit());
        case Op::viaddi:
            return std::format("viaddi(ee.vu0, {}, {}, {});", insn.vit(), insn.vis(),
                               insn.imm5());
        case Op::vilwr:
            return vuWindowAccess("vilwr", insn.dest(), insn.vit(), insn.vis());
        case Op::viswr:
            return vuWindowAccess("viswr", insn.dest(), insn.vit(), insn.vis());
        case Op::vsqi: return vuAccess("vsqi", insn.dest(), insn.vs(), insn.vit());
        case Op::vclipw:
            return std::format("vclip(ee.vu0, {}, {}); ee.vu0.vi[Vu::kClip] = ee.vu0.clip;",
                               insn.vs(), insn.vt());
        case Op::vdiv:
            return delayQuotient(std::format("vdiv(ee.vu0, {}, {}, {}, {});", insn.vs(),
                                             insn.fsf(), insn.vt(), insn.ftf()));
        case Op::vsqrt: return delayQuotient(vu2("vsqrt", insn.vt(), insn.ftf()));
        case Op::vrsqrt:
            return delayQuotient(std::format("vrsqrt(ee.vu0, {}, {}, {}, {});", insn.vs(),
                                             insn.fsf(), insn.vt(), insn.ftf()));
        case Op::vopmula:
            return vuField("vopmula", insn.dest(), insn.vs(), insn.vt());
        case Op::vopmsub:
            return std::format("vopmsub(ee.vu0, 0x{:x}u, {}, {}, {});", insn.dest(), insn.vd(),
                               insn.vs(), insn.vt());

        case Op::vcallms:
            return std::format("vcallms(ee.vu0, mem, {});", insn.imm15());
        case Op::vcallmsr: return std::string{"vcallmsr(ee.vu0, mem);"};

        case Op::teq: return trapPair("teqTaken");
        case Op::tne: return trapPair("tneTaken");
        case Op::tge: return trapPair("tgeTaken");
        case Op::tgeu: return trapPair("tgeuTaken");
        case Op::tlt: return trapPair("tltTaken");
        case Op::tltu: return trapPair("tltuTaken");
        case Op::teqi: return trapImmediate("teqiTaken");
        case Op::tnei: return trapImmediate("tneiTaken");
        case Op::tgei: return trapImmediate("tgeiTaken");
        case Op::tgeiu: return trapImmediate("tgeiuTaken");
        case Op::tlti: return trapImmediate("tltiTaken");
        case Op::tltiu: return trapImmediate("tltiuTaken");

        case Op::vwaitq: return std::string{"ee.vu0.syncQuotient();"};
        case Op::sync_p: return std::string{"ee.vu0.syncFlags();"};
        case Op::vnop:
        case Op::cache:
        case Op::pref:
        case Op::tlbwr:
        case Op::tlbp:
            return std::string{};
        case Op::tlbr: return std::string{"tlbr(ee.cpu);"};
        case Op::tlbwi: return std::string{"tlbwi(ee.cpu, ee.cpu.cop0[cop0::kIndex]);"};

        default: {
            VuForm form;
            if (!findVuForm(op, form)) {
                return std::nullopt;
            }
            const std::string_view name = vuHandlerName(form.kind, form.toAcc);
            const std::string_view from = vuSourceName(form.source);
            if (form.toAcc) {
                return std::format("{}(ee.vu0, VuSource::{}, 0x{:x}u, {}, {});", name, from,
                                   insn.dest(), insn.vs(), insn.vt());
            }
            return std::format("{}(ee.vu0, VuSource::{}, 0x{:x}u, {}, {}, {});", name, from,
                               insn.dest(), insn.vd(), insn.vs(), insn.vt());
        }
    }
}

std::optional<std::string> EeTranslator::emitBranchCondition(Op op, Instruction insn) const {
    const auto r = [](u32 i) { return regName(i); };
    const u32 rs = insn.rs();
    const u32 rt = insn.rt();
    switch (op) {
        case Op::beq: case Op::beql:
            return std::format("beqTaken(ee.cpu, {}, {})", r(rs), r(rt));
        case Op::bne: case Op::bnel:
            return std::format("bneTaken(ee.cpu, {}, {})", r(rs), r(rt));
        case Op::blez: case Op::blezl: return std::format("blezTaken(ee.cpu, {})", r(rs));
        case Op::bgtz: case Op::bgtzl: return std::format("bgtzTaken(ee.cpu, {})", r(rs));
        case Op::bltz: case Op::bltzl:
        case Op::bltzal: case Op::bltzall: return std::format("bltzTaken(ee.cpu, {})", r(rs));
        case Op::bgez: case Op::bgezl:
        case Op::bgezal: case Op::bgezall: return std::format("bgezTaken(ee.cpu, {})", r(rs));
        case Op::bc1t: case Op::bc1tl: return "bc1tTaken(ee.fpu)";
        case Op::bc1f: case Op::bc1fl: return "!bc1tTaken(ee.fpu)";
        case Op::bc0t: case Op::bc0tl: return "ee.cpu.cop0condition";
        case Op::bc0f: case Op::bc0fl: return "!ee.cpu.cop0condition";
        default: return std::nullopt;
    }
}

std::string EeTranslator::readGpr(u32 reg) const {
    return std::format("low(ee.cpu.gpr[{}])", regName(reg));
}

std::string EeTranslator::writeGpr(u32 reg, u32 value) const {
    return std::format("ee.cpu.write64({}, 0x{:08x}u);", regName(reg), value);
}

// Every folded result is the sign extension of its low word, which is what a
// 32-bit write leaves behind.
std::string EeTranslator::writeConstant(u32 reg, u32 value) const {
    return std::format("ee.cpu.write32({}, 0x{:08x}u);", regName(reg), value);
}

// The kernel resumes the guest after the syscall, so the block carries on.
std::optional<Trap> EeTranslator::emitTrap(Instruction insn, const BlockValues& values,
                                           u32 pc) const {
    if (getOp(insn) == Op::eret) {
        return Trap{"returnFromException(ee.cpu);", false};
    }
    if (getOp(insn) == Op::break_) {
        return Trap{std::format("raiseException(ee.cpu, 0x{:08x}u, cop0::kBreakpoint);", pc),
                    false};
    }
    if (getOp(insn) != Op::syscall) {
        return std::nullopt;
    }
    // The number is whatever the block left in the register, which is not
    // always the instruction just before the trap.
    const std::optional<u32> loaded = values.getRegister(kSyscallNumberRegister);
    if (!loaded) {
        return std::nullopt;
    }
    const std::optional<s32> number = static_cast<s32>(*loaded);
    return Trap{std::format("if (!syscall(m, {}, 0x{:08x}u)) return false;", *number, pc + 4),
                true};
}


std::string EeTranslator::countInstructions(u32 count, u32) const {
    return std::format("countExecuted(m, {})", count);
}

std::string EeTranslator::countInstructionsOnly(u32 count) const {
    return std::format("countOnly(m, {});", count);
}

}  // namespace

TranslatedSource translate(Memory& memory, const Program& program, usize units,
                           std::string_view declarationsHeader, const Symbols* symbols,
                           const std::set<u32>& hookable,
                           std::optional<u32> gp,
                           const std::vector<std::pair<u32, u32>>& readOnly) {
    EeTranslator translator(memory, program, symbols);
    translator.setHookable(hookable);
    static std::optional<ReadOnlyImage> image;
    BlockValues values;
    if (gp) {
        values.hold(reg::gp, *gp);
    }
    if (!readOnly.empty()) {
        image.emplace(memory, readOnly);
        values.readFrom(&*image);
    }
    translator.setProgramValues(values);
    return translator.run(units, declarationsHeader);
}

}  // namespace uee
