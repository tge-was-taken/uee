#include "translate.h"

#include "handlers.h"
#include "instruction.h"
#include "translator.h"

#include <algorithm>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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
    const Op op = opOf(insn);
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

usize countCodeWords(const Program& program) {
    if (program.codeEnd <= program.codeStart) {
        throw std::runtime_error(std::format(
            "the program entered at {:08x} declares no executable range, so nothing in it "
            "can be decoded", program.entry));
    }
    return (program.codeEnd - program.codeStart) / 4;
}

class EeTranslator final : public Translator {
public:
    using Translator::Translator;

private:
    std::string_view getMachineType() const override { return "Machine"; }
    std::string_view getContextType() const override { return "EeContext"; }
    std::string_view getStateName() const override { return "ee"; }
    std::string_view getHandlersHeader() const override { return "handlers.h"; }
    std::string_view getRuntimeHeader() const override { return "translate.h"; }

    std::string readGpr(u32 reg) const override;
    std::string writeGpr(u32 reg, u32 value) const override;
    std::optional<std::string> emitOne(Instruction insn) const override;
    std::optional<std::string> emitBranchCondition(Op op, Instruction insn) const override;
    std::optional<Trap> emitTrap(Instruction insn, Instruction previous, u32 pc) const override;
    std::string countInstructions(u32 count) const override;
};

std::optional<std::string> EeTranslator::emitOne(Instruction insn) const {
    const Op op = opOf(insn);
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
    const auto access = [&](const char* name, u32 a, u32 base, s32 offset) {
        return std::format("{}(ee.cpu, mem, {}, {}, {});", name, r(a), r(base), offset);
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
    const auto vuField = [&](const char* name, u32 field, u32 a, u32 b) {
        return std::format("{}(ee.vu0, 0x{:x}u, {}, {});", name, field, a, b);
    };
    const auto vuAccess = [&](const char* name, u32 field, u32 a, u32 b) {
        return std::format("{}(ee.vu0, mem, 0x{:x}u, {}, {});", name, field, a, b);
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
        case Op::addu: return gpr3("addu", rd, rs, rt);
        case Op::sub:
        case Op::subu: return gpr3("subu", rd, rs, rt);
        case Op::dadd:
        case Op::daddu: return gpr3("daddu", rd, rs, rt);
        case Op::dsub:
        case Op::dsubu: return gpr3("dsubu", rd, rs, rt);
        case Op::and_: return gpr3("and_", rd, rs, rt);
        case Op::or_: return gpr3("or_", rd, rs, rt);
        case Op::xor_: return gpr3("xor_", rd, rs, rt);
        case Op::nor: return gpr3("nor", rd, rs, rt);
        case Op::slt: return gpr3("slt", rd, rs, rt);
        case Op::sltu: return gpr3("sltu", rd, rs, rt);
        case Op::movz: return gpr3("movz", rd, rs, rt);
        case Op::movn: return gpr3("movn", rd, rs, rt);

        case Op::addi:
        case Op::addiu: return withImm("addiu", rt, rs, simm);
        case Op::daddi:
        case Op::daddiu: return withImm("daddiu", rt, rs, simm);
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
        case Op::mtc1: return std::format("mtc1(ee.cpu, ee.fpu, {}, {});", r(rt), insn.fs());
        case Op::cfc1:
            return std::format("cfc1(ee.cpu, ee.fpu, {}, {});", r(rt), insn.fs());
        case Op::ctc1:
            return std::format("ctc1(ee.cpu, ee.fpu, {}, {});", r(rt), insn.fs());

        case Op::qmfc2_ni:
            return std::format("qmfc2(ee.cpu, ee.vu0, {}, {});", r(rt), insn.vs());
        case Op::qmtc2_ni:
            return std::format("qmtc2(ee.cpu, ee.vu0, {}, {});", r(rt), insn.vs());
        case Op::cfc2_ni: return std::format("cfc2(ee.cpu, ee.vu0, {}, {});", r(rt), rd);
        case Op::ctc2_ni:
            return std::format("ctc2(ee.cpu, ee, m.vu1(), mem, {}, {});", r(rt), rd);

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
            return std::format("lwc1(ee.cpu, ee.fpu, mem, {}, {}, {});", rt, r(rs), simm);
        case Op::swc1:
            return std::format("swc1(ee.cpu, ee.fpu, mem, {}, {}, {});", rt, r(rs), simm);
        case Op::lqc2:
            return std::format("lqc2(ee.cpu, ee.vu0, mem, {}, {}, {});", rt, r(rs), simm);
        case Op::sqc2:
            return std::format("sqc2(ee.cpu, ee.vu0, mem, {}, {}, {});", rt, r(rs), simm);

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
        case Op::vabs: return vuField("vabs", insn.dest(), insn.vt(), insn.vs());
        case Op::vitof0: return vuField("vitof0", insn.dest(), insn.vt(), insn.vs());
        case Op::vftoi0: return vuField("vftoi0", insn.dest(), insn.vt(), insn.vs());
        case Op::viadd: return vu3("viadd", insn.vid(), insn.vis(), insn.vit());
        case Op::visub: return vu3("visub", insn.vid(), insn.vis(), insn.vit());
        case Op::viand: return vu3("viand", insn.vid(), insn.vis(), insn.vit());
        case Op::vior: return vu3("vior", insn.vid(), insn.vis(), insn.vit());
        case Op::viaddi:
            return std::format("viaddi(ee.vu0, {}, {}, {});", insn.vit(), insn.vis(),
                               insn.imm5());
        case Op::vilwr: return vuAccess("vilwr", insn.dest(), insn.vit(), insn.vis());
        case Op::viswr: return vuAccess("viswr", insn.dest(), insn.vit(), insn.vis());
        case Op::vsqi: return vuAccess("vsqi", insn.dest(), insn.vs(), insn.vit());
        case Op::vsqrt: return vu2("vsqrt", insn.vt(), insn.ftf());
        case Op::vrsqrt:
            return std::format("vrsqrt(ee.vu0, {}, {}, {}, {});", insn.vs(), insn.fsf(),
                               insn.vt(), insn.ftf());
        case Op::vopmula:
            return vuField("vopmula", insn.dest(), insn.vs(), insn.vt());
        case Op::vopmsub:
            return std::format("vopmsub(ee.vu0, 0x{:x}u, {}, {}, {});", insn.dest(), insn.vd(),
                               insn.vs(), insn.vt());

        case Op::vcallms:
            return std::format("vcallms(ee.vu0, mem, {});", insn.imm15());
        case Op::vcallmsr: return std::string{"vcallmsr(ee.vu0, mem);"};

        case Op::vnop:
        case Op::vwaitq:
        case Op::sync_p:
        case Op::cache:
        case Op::pref:
        case Op::tlbwi:
        case Op::tlbwr:
        case Op::tlbr:
        case Op::tlbp:
            return std::string{};

        default: {
            VuForm form;
            if (!vuFormOf(op, form)) {
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

bool alwaysBranches(Op op, Instruction insn) {
    return (op == Op::beq || op == Op::beql) && insn.rs() == insn.rt();
}

std::string EeTranslator::readGpr(u32 reg) const {
    return std::format("low(ee.cpu.gpr[{}])", regName(reg));
}

std::string EeTranslator::writeGpr(u32 reg, u32 value) const {
    return std::format("ee.cpu.write64({}, 0x{:08x}u);", regName(reg), value);
}

// The kernel resumes the guest after the syscall, so the block carries on.
std::optional<Trap> EeTranslator::emitTrap(Instruction insn, Instruction previous, u32 pc) const {
    if (opOf(insn) != Op::syscall) {
        return std::nullopt;
    }
    const std::optional<s32> number = syscallNumberBefore(previous);
    if (!number) {
        return std::nullopt;
    }
    return Trap{std::format("if (!syscall(m, {}, 0x{:08x}u)) return false;", *number, pc + 4),
                true};
}


std::string EeTranslator::countInstructions(u32 count) const {
    return std::format("countPerformanceEvents(ee.cpu, {});", count);
}

}  // namespace

Translator::Translator(Memory& memory, const Program& program)
    : mMemory(memory), mProgram(program), mCovered(countCodeWords(program)) {}

std::string Translator::countInstructions(u32) const {
    return {};
}

std::string Translator::run() {
    seed(mProgram.entry);
    walkSeeds();
    seedFromCodeConstants();
    walkSeeds();
    seedFromDataPointers();
    walkSeeds();
    sweepUnreached();
    promotePrologues();
    for (u32 function : mFunctions) {
        claim(function);
    }
    return render();
}

bool Translator::inCode(u32 address) const {
    return (address & 3) == 0 && address >= mProgram.codeStart &&
           address + 4 <= mProgram.codeEnd;
}

void Translator::seed(u32 function) {
    mFunctions.insert(function);
    mLeaders.insert(function);
    mSeeds.push_back(function);
}

void Translator::walkSeeds() {
    discoverFrom(std::exchange(mSeeds, {}));
}

void Translator::cover(u32 pc) {
    if (inCode(pc)) {
        mCovered[(pc - mProgram.codeStart) / 4] = true;
    }
}

bool Translator::reached(u32 pc) const {
    return mCovered[(pc - mProgram.codeStart) / 4];
}

// Absorbing one into its caller would nest a guest frame in a host frame
// that does not own it.
bool Translator::allocatesFrame(u32 pc) {
    const std::optional<Instruction> insn = fetch(pc);
    return insn && opOf(*insn) == Op::addiu && insn->rt() == reg::sp &&
           insn->rs() == reg::sp && insn->simm() < 0;
}

// A jump table's entries are blocks, not functions.
void Translator::seedFromDataPointers() {
    const u32 first = (mProgram.imageStart + 3) & ~3u;
    for (u32 address = first; address + 4 <= mProgram.imageEnd; address += 4) {
        if (address >= mProgram.codeStart && address < mProgram.codeEnd) {
            continue;
        }
        if (mTableWords.count(address)) {
            continue;
        }
        const std::optional<Instruction> word = fetch(address);
        if (!word) {
            return;
        }
        if (inCode(word->word) && !mFunctions.count(word->word)) {
            seed(word->word);
        }
    }
}

// Landing on an instruction is the only evidence the encoding leaves.
void Translator::seedFromCodeConstants() {
    u32 upper[32] = {};
    bool haveUpper[32] = {};
    bool endsBlock = false;
    for (u32 pc = mProgram.codeStart; pc + 4 <= mProgram.codeEnd; pc += 4) {
        const std::optional<Instruction> insn = fetch(pc);
        if (!insn) {
            return;
        }
        const Op op = opOf(*insn);
        const bool transfers = controlFlow(op, *insn, pc).flow != Flow::Next;
        if (std::exchange(endsBlock, transfers)) {
            std::fill(std::begin(haveUpper), std::end(haveUpper), false);
        }
        if (transfers) {
            continue;
        }
        if (op == Op::lui) {
            upper[insn->rt()] = insn->imm() << 16;
            haveUpper[insn->rt()] = true;
            continue;
        }
        if (op == Op::addiu || op == Op::daddiu || op == Op::ori) {
            const u32 low = op == Op::ori ? insn->imm() : static_cast<u32>(insn->simm());
            const u32 address = upper[insn->rs()] + low;
            if (haveUpper[insn->rs()] && inCode(address) && !reached(address) &&
                !mFunctions.count(address)) {
                seed(address);
            }
        }
        haveUpper[insn->rt()] = false;
        if ((insn->word >> 26) == 0) {
            haveUpper[insn->rd()] = false;
        }
    }
}

// Whatever the walk never reached is a function nothing calls.
void Translator::sweepUnreached() {
    for (u32 pc = mProgram.codeStart; pc + 4 <= mProgram.codeEnd; pc += 4) {
        if (reached(pc)) {
            continue;
        }
        const std::optional<Instruction> insn = fetch(pc);
        if (!insn) {
            return;
        }
        if (insn->word == 0) {
            continue;
        }
        seed(pc);
        walkSeeds();
    }
}

void Translator::promotePrologues() {
    for (u32 leader : mLeaders) {
        if (allocatesFrame(leader)) {
            mFunctions.insert(leader);
        }
    }
}

// Recovered, the jump stays local; a host call would make it a new frame.
std::vector<u32> Translator::recoverJumpTable(u32 jrPc, u32 source) {
    u32 base = 0, count = 0, pendingLow = 0, pendingReg = 32;
    s32 loadOffset = 0;
    bool haveBase = false, haveCount = false, haveOffset = false;
    for (s32 back = 1; back <= 16 && !(haveBase && haveCount); ++back) {
        const std::optional<Instruction> insn = fetch(jrPc - 4 * static_cast<u32>(back));
        if (!insn) {
            break;
        }
        switch (opOf(*insn)) {
            // Only the load that fills the jump register locates the table.
            case Op::lw:
                if (insn->rt() == source && !haveOffset) {
                    loadOffset = insn->simm();
                    haveOffset = true;
                }
                break;
            case Op::addiu:
                if (insn->rt() == insn->rs()) {
                    pendingLow = static_cast<u32>(insn->simm());
                    pendingReg = insn->rt();
                }
                break;
            case Op::lui:
                if (insn->rt() == pendingReg) {
                    base = (insn->imm() << 16) + pendingLow;
                    haveBase = true;
                }
                break;
            case Op::sltiu:
                count = insn->imm();
                haveCount = true;
                break;
            default:
                break;
        }
    }
    std::vector<u32> targets;
    if (!haveBase || !haveCount || !haveOffset || count == 0 || count > 1024) {
        return targets;
    }
    const u32 table = base + static_cast<u32>(loadOffset);
    for (u32 i = 0; i < count; ++i) {
        const std::optional<Instruction> entry = fetch(table + 4 * i);
        if (!entry || !inCode(entry->word)) {
            return {};
        }
        targets.push_back(entry->word);
    }
    for (u32 i = 0; i < count; ++i) {
        mTableWords.insert(table + 4 * i);
    }
    return targets;
}

std::optional<Instruction> Translator::fetch(u32 pc) {
    if (mMemory.fault()) {
        return std::nullopt;
    }
    const Instruction insn{mMemory.read32(pc)};
    return mMemory.fault() ? std::nullopt : std::optional(insn);
}

void Translator::discoverFrom(std::vector<u32> seeds) {
    std::vector<u32> pending = std::move(seeds);
    std::set<u32>& walked = mWalked;
    while (!pending.empty()) {
        const u32 start = pending.back();
        pending.pop_back();
        if (!walked.insert(start).second) {
            continue;
        }
        const auto reach = [&](u32 target) {
            if (!inCode(target)) {
                return;
            }
            mLeaders.insert(target);
            pending.push_back(target);
        };
        u32 pc = start;
        for (;;) {
            const std::optional<Instruction> insn = fetch(pc);
            if (!insn) {
                break;
            }
            cover(pc);
            const Op op = opOf(*insn);
            const ControlFlow cf = controlFlow(op, *insn, pc);
            if (cf.flow == Flow::Next || (cf.flow == Flow::Trap && op == Op::syscall)) {
                pc += 4;
                if (mLeaders.count(pc)) {
                    break;
                }
                continue;
            }
            if (cf.flow != Flow::Trap) {
                cover(pc + 4);
            }
            if (cf.flow == Flow::Branch || cf.flow == Flow::BranchLikely) {
                reach(cf.target);
                if (!alwaysBranches(op, *insn)) {
                    reach(pc + 8);
                }
            } else if (cf.flow == Flow::Jump) {
                if (cf.link != 0) {
                    if (inCode(cf.target)) {
                        mFunctions.insert(cf.target);
                    }
                    reach(cf.target);
                    reach(pc + 8);
                } else {
                    reach(cf.target);
                }
            } else if (cf.flow == Flow::JumpRegister) {
                if (cf.link != 0) {
                    reach(pc + 8);
                } else if (cf.source != 31) {
                    const std::vector<u32> targets = recoverJumpTable(pc, cf.source);
                    mJumpTables[pc] = targets;
                    for (u32 target : targets) {
                        reach(target);
                    }
                }
            }
            break;
        }
    }
}

// A leader two functions reach is emitted into both, so every branch target
// stays inside the function that branches to it.
void Translator::claim(u32 function) {
    std::vector<u32> pending{function};
    std::set<u32>& owned = mOwned[function];
    while (!pending.empty()) {
        const u32 leader = pending.back();
        pending.pop_back();
        if (!mLeaders.count(leader) || !owned.insert(leader).second) {
            continue;
        }
        if (leader != function && mFunctions.count(leader)) {
            owned.erase(leader);
            continue;
        }
        for (u32 successor : successorsOf(leader)) {
            pending.push_back(successor);
        }
    }
}

std::vector<u32> Translator::successorsOf(u32 leader) {
    std::vector<u32> out;
    u32 pc = leader;
    for (;;) {
        const std::optional<Instruction> insn = fetch(pc);
        if (!insn) {
            return out;
        }
        const Op op = opOf(*insn);
        const ControlFlow cf = controlFlow(op, *insn, pc);
        if (cf.flow == Flow::Next || (cf.flow == Flow::Trap && op == Op::syscall)) {
            pc += 4;
            if (mLeaders.count(pc)) {
                out.push_back(pc);
                return out;
            }
            continue;
        }
        if (cf.flow == Flow::Branch || cf.flow == Flow::BranchLikely) {
            out.push_back(cf.target);
            if (!alwaysBranches(op, *insn)) {
                out.push_back(pc + 8);
            }
        } else if (cf.flow == Flow::Jump) {
            out.push_back(cf.link != 0 ? pc + 8 : cf.target);
        } else if (cf.flow == Flow::JumpRegister) {
            if (cf.link != 0) {
                out.push_back(pc + 8);
            } else if (const auto table = mJumpTables.find(pc); table != mJumpTables.end()) {
                for (u32 target : table->second) {
                    out.push_back(target);
                }
            }
        }
        return out;
    }
}

bool Translator::owns(u32 function, u32 target) const {
    const auto it = mOwned.find(function);
    return it != mOwned.end() && it->second.count(target) != 0;
}

std::string Translator::go(u32 target, u32 function) const {
    if (owns(function, target)) {
        return std::format("goto L_{:08x};", target);
    }
    if (mOwned.count(target)) {
        return std::format("func_{0:08x}(m);\n    return leave({1}, {1}.cpu.pc, returnTo);",
                           target, getStateName());
    }
    return std::format("return leave({}, 0x{:08x}u, returnTo);", getStateName(), target);
}

// A block that ends by jumping to the one written next can just run into it.
void Translator::dropJumpToNext(std::string& text, u32 next) {
    const std::string only = std::format("    goto L_{:08x};\n", next);
    if (text == only) {
        text.clear();
        return;
    }
    const std::string tail = "\n" + only;
    if (text.size() > tail.size() &&
        text.compare(text.size() - tail.size(), tail.size(), tail) == 0) {
        text.resize(text.size() - tail.size() + 1);
    }
}

std::string Translator::annotate(const std::string& statement, u32 pc, Instruction insn,
                            s32 depth) {
    const std::string line = std::string(depth * 4, ' ') + statement;
    return std::format("{:<{}}// {:08x}  {}", line, kCommentColumn, pc,
                       disassemble(insn, pc));
}

// An interpreted instruction that did not land on the next one never
// completed: it was unimplemented, faulted, or transferred control. Emitted
// code cannot carry on past any of those.
std::string Translator::stepAt(u32 pc, s32 depth) const {
    const std::string indent(4 * static_cast<usize>(depth), ' ');
    return std::format("{0}step(m, 0x{2:08x}u);\n{0}if ({1}.cpu.pc != 0x{3:08x}u) {{\n"
                       "{0}    return leave({1}, {1}.cpu.pc, returnTo);\n{0}}}\n",
                       indent, getStateName(), pc, pc + 4);
}

std::string Translator::emitBeforeInstructionAt(s32 depth) const {
    const std::string statement = emitBeforeInstruction();
    if (statement.empty()) {
        return {};
    }
    return std::format("{}{}\n", std::string(4 * static_cast<usize>(depth), ' '), statement);
}

// A delay slot that can transfer control of its own resolves the pc rather
// than falling through, so emitted code cannot carry on to a target it
// picked before the slot ran.
bool Translator::delaySlotTransfers(u32 pc) {
    const std::optional<Instruction> delayed = fetch(pc + 4);
    return delayed && controlFlow(opOf(*delayed), *delayed, pc + 4).flow != Flow::Next;
}

// Hands the slot to the driver with the pending transfer already in pc, the
// same way the interpreter resolves the pair.
std::string Translator::emitTransferringSlot(const std::string& pending, u32 pc, s32 depth) const {
    const std::string indent(4 * static_cast<usize>(depth), ' ');
    return std::format("{0}{1}.cpu.pc = {2};\n{0}stepDelaySlot(m, 0x{3:08x}u);\n"
                       "{0}return leave({1}, {1}.cpu.pc, returnTo);\n",
                       indent, getStateName(), pending, pc + 4);
}

// The performance counters advance a block at a time here where the
// interpreter advances one instruction at a time; the totals agree at every
// block boundary. Instructions handed to step or call are left out because
// the interpreter counts those itself.
std::string Translator::counted(const std::string& body) const {
    if (mInlineInstructions == 0) {
        return body;
    }
    const std::string statement = countInstructions(mInlineInstructions);
    if (statement.empty()) {
        return body;
    }
    return std::format("    {}\n", statement) + body;
}

std::string Translator::emitBlock(u32 leader, u32 function) {
    std::string out;
    mInlineInstructions = 0;
    u32 pc = leader;
    for (;;) {
        const std::optional<Instruction> insn = fetch(pc);
        if (!insn) {
            return counted(out + std::format("    return leave({}, 0x{:08x}u, returnTo);\n",
                                            getStateName(), pc));
        }
        const Op op = opOf(*insn);
        const ControlFlow cf = controlFlow(op, *insn, pc);

        if (cf.flow == Flow::Next || (cf.flow == Flow::Trap && op == Op::syscall)) {
            if (op == Op::syscall) {
                const std::optional<Instruction> previous = fetch(pc - 4);
                const std::optional<Trap> trap =
                    emitTrap(*insn, previous.value_or(Instruction{}), pc);
                if (trap) {
                    out += emitBeforeInstructionAt(1);
                    out += annotate(trap->statement, pc, *insn, 1) + "\n";
                    ++mInlineInstructions;
                    if (!trap->continues) {
                        return counted(out + std::format("    return leave({0}, {0}.cpu.pc, "
                                                         "returnTo);\n", getStateName()));
                    }
                    pc += 4;
                    if (mLeaders.count(pc)) {
                        return counted(out + std::format("    {}\n", go(pc, function)));
                    }
                    continue;
                }
            }
            if (const std::optional<std::string> statement = emitOne(*insn)) {
                ++mInlineInstructions;
                out += emitBeforeInstructionAt(1);
                if (!statement->empty()) {
                    out += annotate(*statement, pc, *insn, 1) + "\n";
                }
            } else {
                out += annotate(std::format("step(m, 0x{:08x}u);", pc), pc, *insn, 1) + "\n";
                out += std::format("    if ({0}.cpu.pc != 0x{1:08x}u) {{\n"
                                   "        return leave({0}, {0}.cpu.pc, returnTo);\n    }}\n",
                                   getStateName(), pc + 4);
            }
            pc += 4;
            if (mLeaders.count(pc)) {
                return counted(out + std::format("    {}\n", go(pc, function)));
            }
            continue;
        }
        if (cf.flow == Flow::Trap) {
            out += annotate(std::format("step(m, 0x{:08x}u);", pc), pc, *insn, 1) + "\n";
            out += std::format("    call(m, {0}.cpu.pc, {1});\n    return leave({0}, "
                               "{0}.cpu.pc, returnTo);\n",
                               getStateName(), readGpr(reg::ra));
            return counted(out);
        }
        return counted(out + emitExit(cf, op, *insn, pc, function));
    }
}

// Both orders matter: a program may name the same register twice.
std::string Translator::emitExit(const ControlFlow& cf, Op op, Instruction insn, u32 pc,
                                 u32 function) {
    const std::optional<Instruction> delayed = fetch(pc + 4);
    const std::optional<std::string> inlineSlot = delayed ? emitOne(*delayed) : std::nullopt;
    const auto slotAt = [&](s32 depth) {
        if (!inlineSlot) {
            return stepAt(pc + 4, depth);
        }
        if (inlineSlot->empty()) {
            return emitBeforeInstructionAt(depth);
        }
        return emitBeforeInstructionAt(depth) +
               annotate(*inlineSlot, pc + 4, *delayed, depth) + "  (delay slot)\n";
    };
    const bool slotTransfers = delaySlotTransfers(pc);

    // The transfer runs whatever happens. Its delay slot runs too unless a
    // likely branch nullifies it, which is only known at run time, so that
    // one is counted where it actually executes.
    ++mInlineInstructions;
    const bool slotAlwaysRuns = cf.flow != Flow::BranchLikely;
    if (inlineSlot && slotAlwaysRuns) {
        ++mInlineInstructions;
    }
    const std::string slotCount =
        inlineSlot && !slotAlwaysRuns ? countInstructions(1) : std::string{};
    const std::string countSlot =
        slotCount.empty() ? std::string{} : std::format("            {}\n", slotCount);

    // The link register is written after the target and the condition have
    // been read, because a program may name the same register twice.
    const auto linkAt = [&](s32 depth) {
        return cf.link == 0 ? std::string{}
                            : std::format("{}{}\n",
                                          std::string(4 * static_cast<usize>(depth), ' '),
                                          writeGpr(cf.link, pc + 8));
    };

    std::string out = emitBeforeInstructionAt(1);

    if (cf.flow == Flow::JumpRegister) {
        out += "    {\n";
        out += annotate(std::format("const u32 destination = {};",
                                    readGpr(cf.source)), pc, insn, 2) + "\n";
        out += linkAt(2);
        if (slotTransfers) {
            return out + emitTransferringSlot("destination", pc, 2) + "    }\n";
        }
        out += slotAt(2);
        if (cf.link != 0) {
            out += std::format("        {}.cpu.pc = destination;\n", getStateName());
            out += std::format("        if (!call(m, destination, 0x{:08x}u)) return false;\n"
                               "    }}\n", pc + 8);
            out += std::format("    {}\n", go(pc + 8, function));
        } else if (cf.source == 31) {
            out += std::format("        return leave({}, destination, returnTo);\n    }}\n",
                               getStateName());
        } else {
            out += std::format("        {}.cpu.pc = destination;\n", getStateName());
            const auto table = mJumpTables.find(pc);
            if (table != mJumpTables.end() && !table->second.empty()) {
                out += "        switch (destination) {\n";
                std::set<u32> seen;
                for (u32 target : table->second) {
                    if (!seen.insert(target).second) {
                        continue;
                    }
                    out += std::format("            case 0x{:08x}u: {}\n", target,
                                       go(target, function));
                }
                out += "            default: break;\n        }\n";
            }
            out += std::format("        return leave({}, destination, returnTo);\n    }}\n",
                               getStateName());
        }
        return out;
    }

    if (cf.flow == Flow::Jump) {
        if (slotTransfers && cf.link == 0) {
            return out + emitTransferringSlot(std::format("0x{:08x}u", cf.target), pc, 1);
        }
        out += linkAt(1);
        out += slotAt(1);
        if (cf.link == 0) {
            out += annotate(go(cf.target, function), pc, insn, 1) + "\n";
            return out;
        }
        if (mFunctions.count(cf.target) && mOwned.count(cf.target)) {
            out += annotate(std::format("if (!func_{:08x}(m)) return false;", cf.target),
                            pc, insn, 1) + "\n";
        } else {
            out += annotate(std::format("if (!call(m, 0x{:08x}u, 0x{:08x}u)) return false;",
                                        cf.target, pc + 8), pc, insn, 1) + "\n";
        }
        out += std::format("    {}\n", go(pc + 8, function));
        return out;
    }

    if (alwaysBranches(op, insn)) {
        if (slotTransfers) {
            return out + emitTransferringSlot(std::format("0x{:08x}u", cf.target), pc, 1);
        }
        out += slotAt(1);
        out += annotate(go(cf.target, function), pc, insn, 1) + "\n";
        return out;
    }

    const std::optional<std::string> condition = emitBranchCondition(op, insn);
    out += "    {\n";
    out += annotate(std::format("const bool taken = {};", condition ? *condition : "false"),
                    pc, insn, 2) + "\n";
    out += linkAt(2);
    if (cf.flow == Flow::BranchLikely) {
        out += "        if (taken) {\n";
        if (slotTransfers) {
            out += emitTransferringSlot(std::format("0x{:08x}u", cf.target), pc, 3);
            out += std::format("        }}\n    }}\n    {}\n", go(pc + 8, function));
            return out;
        }
        out += countSlot;
        out += slotAt(3);
        out += std::format("            {}\n        }}\n    }}\n", go(cf.target, function));
        out += std::format("    {}\n", go(pc + 8, function));
        return out;
    }
    if (slotTransfers) {
        out += emitTransferringSlot(
            std::format("taken ? 0x{:08x}u : 0x{:08x}u", cf.target, pc + 8), pc, 2);
        return out + "    }\n";
    }
    out += slotAt(2);
    out += std::format("        if (taken) {{\n            {}\n        }}\n    }}\n",
                       go(cf.target, function));
    out += std::format("    {}\n", go(pc + 8, function));
    return out;
}

std::string Translator::render() {
    std::string out;
    out += std::format("#include \"{}\"\n#include \"{}\"\n\n", getHandlersHeader(),
                       getRuntimeHeader());
    out += "#include <algorithm>\n\n";
    out += "namespace uee {\n";
    out += "using namespace reg;\n";

    for (const auto& [function, leaders] : mOwned) {
        out += std::format("\nstatic bool func_{:08x}({}&);", function, getMachineType());
    }
    out += "\n";

    for (const auto& [function, leaders] : mOwned) {
        std::vector<u32> order{function};
        for (u32 leader : leaders) {
            if (leader != function) {
                order.push_back(leader);
            }
        }
        std::map<u32, std::string> blocks;
        for (u32 leader : order) {
            blocks[leader] = emitBlock(leader, function);
        }
        for (usize i = 0; i + 1 < order.size(); ++i) {
            dropJumpToNext(blocks[order[i]], order[i + 1]);
        }
        std::string all;
        for (u32 leader : order) {
            all += blocks[leader];
        }
        std::string body;
        for (u32 leader : order) {
            if (all.find(std::format("goto L_{:08x};", leader)) != std::string::npos) {
                body += std::format("\nL_{:08x}:\n", leader);
            } else {
                body += "\n";
            }
            body += blocks[leader];
        }
        out += std::format("\nstatic bool func_{:08x}({}& m) {{\n", function,
                           getMachineType());
        out += std::format("    {0}& {1} = m.{1}();\n", getContextType(), getStateName());
        if (body.find("mem") != std::string::npos) {
            out += "    Memory& mem = m.memory;\n";
        }
        if (body.find("returnTo") != std::string::npos) {
            out += std::format("    const u32 returnTo = {};\n", readGpr(reg::ra));
        }
        out += body;
        out += "}\n";
    }

    // The addresses are data, not control flow the optimiser has to chew through.
    out += std::format("\nstruct Function {{\n    u32 address;\n"
                       "    bool (*run)({}&);\n}};\n", getMachineType());
    out += "\nconstexpr Function kFunctions[] = {\n";
    for (const auto& [function, leaders] : mOwned) {
        out += std::format("    {{0x{:08x}u, func_{:08x}}},\n", function, function);
    }
    out += "};\n";

    out += std::format("\nbool runFunction({}& m, u32 pc) {{\n", getMachineType());
    out += "    const Function* found = std::lower_bound(\n";
    out += "        std::begin(kFunctions), std::end(kFunctions), pc,\n";
    out += "        [](const Function& entry, u32 address) "
           "{ return entry.address < address; });\n";
    out += "    if (found == std::end(kFunctions) || found->address != pc) {\n";
    out += "        return false;\n";
    out += "    }\n";
    out += "    found->run(m);\n";
    out += "    return true;\n";
    out += "}\n\n";
    out += "}  // namespace uee\n";
    return out;
}

std::string translate(Memory& memory, const Program& program) {
    return EeTranslator(memory, program).run();
}

}  // namespace uee
