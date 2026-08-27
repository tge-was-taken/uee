#include "interpreter.h"

#include <bit>

namespace uee {
namespace {

constexpr u32 low(const Reg128& r) {
    return static_cast<u32>(r.lo);
}

constexpr s64 signed64(const Reg128& r) {
    return static_cast<s64>(r.lo);
}

constexpr Reg128 lanes(u32 w0, u32 w1, u32 w2, u32 w3) {
    return Reg128{(static_cast<u64>(w1) << 32) | w0, (static_cast<u64>(w3) << 32) | w2};
}

constexpr u16 half(const Reg128& r, s32 i) {
    return static_cast<u16>(r.word(i / 2) >> (16 * (i & 1)));
}

constexpr u8 byte(const Reg128& r, s32 i) {
    return static_cast<u8>(r.word(i / 4) >> (8 * (i & 3)));
}

constexpr Reg128 fromHalves(const u16 h[8]) {
    Reg128 r{};
    for (s32 i = 0; i < 4; ++i) {
        r.setWord(i, static_cast<u32>(h[2 * i]) | (static_cast<u32>(h[2 * i + 1]) << 16));
    }
    return r;
}

constexpr Reg128 fromBytes(const u8 b[16]) {
    Reg128 r{};
    for (s32 i = 0; i < 4; ++i) {
        r.setWord(i, static_cast<u32>(b[4 * i]) | (static_cast<u32>(b[4 * i + 1]) << 8) |
                         (static_cast<u32>(b[4 * i + 2]) << 16) |
                         (static_cast<u32>(b[4 * i + 3]) << 24));
    }
    return r;
}

// The R5900 puts the low word in rd as well as in LO.
void setMultiplyResult(R5900& cpu, u32 rd, u64 product, bool pipeline1) {
    const u32 lowWord = static_cast<u32>(product);
    const u32 highWord = static_cast<u32>(product >> 32);
    Reg128& lo = cpu.lo;
    Reg128& hi = cpu.hi;
    const u64 loValue = static_cast<u64>(static_cast<s64>(static_cast<s32>(lowWord)));
    const u64 hiValue = static_cast<u64>(static_cast<s64>(static_cast<s32>(highWord)));
    if (pipeline1) {
        lo.hi = loValue;
        hi.hi = hiValue;
    } else {
        lo.lo = loValue;
        hi.lo = hiValue;
    }
    cpu.write32(rd, lowWord);
}

void setDivideResult(R5900& cpu, u32 quotient, u32 remainder, bool pipeline1) {
    const u64 q = static_cast<u64>(static_cast<s64>(static_cast<s32>(quotient)));
    const u64 r = static_cast<u64>(static_cast<s64>(static_cast<s32>(remainder)));
    if (pipeline1) {
        cpu.lo.hi = q;
        cpu.hi.hi = r;
    } else {
        cpu.lo.lo = q;
        cpu.hi.lo = r;
    }
}

// MIPS leaves signed division by zero and INT_MIN / -1 undefined; the reference
// produces these, so they are reproduced rather than guessed at.
void divideSigned(R5900& cpu, s32 a, s32 b, bool pipeline1) {
    if (b == 0) {
        setDivideResult(cpu, a < 0 ? 1u : 0xFFFFFFFFu, static_cast<u32>(a), pipeline1);
    } else if (b == -1 && a == static_cast<s32>(0x80000000)) {
        setDivideResult(cpu, 0x80000000u, 0, pipeline1);
    } else {
        setDivideResult(cpu, static_cast<u32>(a / b), static_cast<u32>(a % b), pipeline1);
    }
}

// HI and LO each hold a sign-extended 32-bit half of the accumulator.
constexpr u64 multiplyAccumulator(const R5900& cpu) {
    return (static_cast<u64>(cpu.hi.word(0)) << 32) | cpu.lo.word(0);
}

// EPC keeps the address of the first exception, so a fault inside a handler
// does not lose where the handler was entered from.
void raiseException(R5900& cpu, u32 pc, u32 code) {
    u32& status = cpu.cop0[cop0::kStatus];
    u32& cause = cpu.cop0[cop0::kCause];
    cause = (cause & ~cop0::kCauseCodeMask) | (code << cop0::kCauseCodeShift);
    if ((status & cop0::kExceptionLevel) == 0) {
        cpu.cop0[cop0::kEpc] = pc;
        status |= cop0::kExceptionLevel;
    }
    cpu.pc = (status & cop0::kBootExceptionVectors) ? cop0::kBootExceptionVector
                                                    : cop0::kExceptionVector;
}

void returnFromException(R5900& cpu) {
    u32& status = cpu.cop0[cop0::kStatus];
    const bool fromError = (status & cop0::kErrorLevel) != 0;
    cpu.pc = cpu.cop0[fromError ? cop0::kErrorEpc : cop0::kEpc];
    status &= ~(fromError ? cop0::kErrorLevel : cop0::kExceptionLevel);
}

void divideUnsigned(R5900& cpu, u32 a, u32 b, bool pipeline1) {
    if (b == 0) {
        setDivideResult(cpu, 0xFFFFFFFFu, a, pipeline1);
    } else {
        setDivideResult(cpu, a / b, a % b, pipeline1);
    }
}

}  // namespace

Status Interpreter::execute(Instruction insn) {
    R5900& cpu = machine.cpu;
    Memory& memory = mMemory;
    const Slot& slot = decode(insn);
    const Reg128& rs = cpu.gpr[insn.rs()];
    const Reg128& rt = cpu.gpr[insn.rt()];
    const u32 rd = insn.rd();
    const u32 sa = insn.sa();
    const u32 address = low(rs) + static_cast<u32>(insn.simm());
    const u32 pc = cpu.pc;
    const u32 target = pc + 4 + (static_cast<u32>(insn.simm()) << 2);
    // a branch resolves the PC itself; everything else falls through to pc + 4
    cpu.pc = pc + 4;
    mDelaySlot = false;

    const auto branchIf = [&](bool taken) {
        cpu.pc = taken ? target : pc + 8;
        mDelaySlot = true;
    };
    const auto branchLikelyIf = [&](bool taken) {
        cpu.pc = taken ? target : pc + 8;
        mDelaySlot = taken;
    };
    const auto jumpTo = [&](u32 destination) {
        cpu.pc = destination;
        mDelaySlot = true;
    };
    // the link register holds the 32-bit return address unextended
    const auto link = [&](u32 reg) { cpu.write64(reg, pc + 8); };

    switch (slot.op) {
        case Op::Invalid:
            return Status::Undecodable;

        case Op::sll: cpu.write32(rd, low(rt) << sa); break;
        case Op::srl: cpu.write32(rd, low(rt) >> sa); break;
        case Op::sra: cpu.write32(rd, static_cast<u32>(static_cast<s32>(low(rt)) >> sa)); break;
        case Op::dsll: cpu.write64(rd, rt.lo << sa); break;
        case Op::dsrl: cpu.write64(rd, rt.lo >> sa); break;
        case Op::dsra: cpu.write64(rd, static_cast<u64>(signed64(rt) >> sa)); break;
        case Op::dsll32: cpu.write64(rd, rt.lo << (sa + 32)); break;
        case Op::dsrl32: cpu.write64(rd, rt.lo >> (sa + 32)); break;
        case Op::dsra32: cpu.write64(rd, static_cast<u64>(signed64(rt) >> (sa + 32))); break;

        case Op::sllv: cpu.write32(rd, low(rt) << (low(rs) & 31)); break;
        case Op::srlv: cpu.write32(rd, low(rt) >> (low(rs) & 31)); break;
        case Op::srav:
            cpu.write32(rd, static_cast<u32>(static_cast<s32>(low(rt)) >> (low(rs) & 31)));
            break;
        case Op::dsllv: cpu.write64(rd, rt.lo << (low(rs) & 63)); break;
        case Op::dsrlv: cpu.write64(rd, rt.lo >> (low(rs) & 63)); break;
        case Op::dsrav: cpu.write64(rd, static_cast<u64>(signed64(rt) >> (low(rs) & 63))); break;

        case Op::add:
        case Op::addu: cpu.write32(rd, low(rs) + low(rt)); break;
        case Op::sub:
        case Op::subu: cpu.write32(rd, low(rs) - low(rt)); break;
        case Op::dadd:
        case Op::daddu: cpu.write64(rd, rs.lo + rt.lo); break;
        case Op::dsub:
        case Op::dsubu: cpu.write64(rd, rs.lo - rt.lo); break;
        case Op::and_: cpu.write64(rd, rs.lo & rt.lo); break;
        case Op::or_: cpu.write64(rd, rs.lo | rt.lo); break;
        case Op::xor_: cpu.write64(rd, rs.lo ^ rt.lo); break;
        case Op::nor: cpu.write64(rd, ~(rs.lo | rt.lo)); break;
        case Op::slt: cpu.write64(rd, signed64(rs) < signed64(rt) ? 1 : 0); break;
        case Op::sltu: cpu.write64(rd, rs.lo < rt.lo ? 1 : 0); break;

        case Op::addi:
        case Op::addiu: cpu.write32(insn.rt(), low(rs) + static_cast<u32>(insn.simm())); break;
        case Op::daddi:
        case Op::daddiu:
            cpu.write64(insn.rt(), rs.lo + static_cast<u64>(static_cast<s64>(insn.simm())));
            break;
        case Op::andi: cpu.write64(insn.rt(), rs.lo & insn.imm()); break;
        case Op::ori: cpu.write64(insn.rt(), rs.lo | insn.imm()); break;
        case Op::xori: cpu.write64(insn.rt(), rs.lo ^ insn.imm()); break;
        case Op::slti:
            cpu.write64(insn.rt(), signed64(rs) < insn.simm() ? 1 : 0);
            break;
        case Op::sltiu:
            cpu.write64(insn.rt(), rs.lo < static_cast<u64>(static_cast<s64>(insn.simm())) ? 1 : 0);
            break;
        case Op::lui: cpu.write32(insn.rt(), insn.imm() << 16); break;

        case Op::pand: cpu.write128(rd, Reg128{rs.lo & rt.lo, rs.hi & rt.hi}); break;
        case Op::por: cpu.write128(rd, Reg128{rs.lo | rt.lo, rs.hi | rt.hi}); break;
        case Op::pxor: cpu.write128(rd, Reg128{rs.lo ^ rt.lo, rs.hi ^ rt.hi}); break;
        case Op::pnor: cpu.write128(rd, Reg128{~(rs.lo | rt.lo), ~(rs.hi | rt.hi)}); break;
        case Op::pcpyld: cpu.write128(rd, Reg128{rt.lo, rs.lo}); break;
        case Op::pcpyud: cpu.write128(rd, Reg128{rs.hi, rt.hi}); break;
        case Op::pextlw:
            cpu.write128(rd, lanes(rt.word(0), rs.word(0), rt.word(1), rs.word(1)));
            break;
        case Op::pextuw:
            cpu.write128(rd, lanes(rt.word(2), rs.word(2), rt.word(3), rs.word(3)));
            break;
        case Op::paddw:
            cpu.write128(rd,
                     lanes(rs.word(0) + rt.word(0), rs.word(1) + rt.word(1),
                           rs.word(2) + rt.word(2), rs.word(3) + rt.word(3)));
            break;
        case Op::psubw:
            cpu.write128(rd,
                     lanes(rs.word(0) - rt.word(0), rs.word(1) - rt.word(1),
                           rs.word(2) - rt.word(2), rs.word(3) - rt.word(3)));
            break;

        case Op::mult:
            setMultiplyResult(cpu, rd,
                              static_cast<u64>(static_cast<s64>(static_cast<s32>(low(rs))) *
                                               static_cast<s64>(static_cast<s32>(low(rt)))),
                              false);
            break;
        case Op::multu:
            setMultiplyResult(cpu, rd,
                              static_cast<u64>(low(rs)) * static_cast<u64>(low(rt)), false);
            break;
        case Op::mult1:
            setMultiplyResult(cpu, rd,
                              static_cast<u64>(static_cast<s64>(static_cast<s32>(low(rs))) *
                                               static_cast<s64>(static_cast<s32>(low(rt)))),
                              true);
            break;
        case Op::multu1:
            setMultiplyResult(cpu, rd,
                              static_cast<u64>(low(rs)) * static_cast<u64>(low(rt)), true);
            break;
        case Op::madd:
            setMultiplyResult(cpu, rd,
                              multiplyAccumulator(cpu) +
                                  static_cast<u64>(static_cast<s64>(static_cast<s32>(low(rs))) *
                                                   static_cast<s64>(static_cast<s32>(low(rt)))),
                              false);
            break;
        case Op::div:
            divideSigned(cpu, static_cast<s32>(low(rs)), static_cast<s32>(low(rt)), false);
            break;
        case Op::divu: divideUnsigned(cpu, low(rs), low(rt), false); break;
        case Op::div1:
            divideSigned(cpu, static_cast<s32>(low(rs)), static_cast<s32>(low(rt)), true);
            break;
        case Op::divu1: divideUnsigned(cpu, low(rs), low(rt), true); break;
        case Op::mthi: cpu.hi.lo = rs.lo; break;
        case Op::mtlo: cpu.lo.lo = rs.lo; break;
        case Op::mthi1: cpu.hi.hi = rs.lo; break;
        case Op::mtlo1: cpu.lo.hi = rs.lo; break;

        case Op::movz:
            if (rt.lo == 0) {
                cpu.write64(rd, rs.lo);
            }
            break;
        case Op::movn:
            if (rt.lo != 0) {
                cpu.write64(rd, rs.lo);
            }
            break;

        case Op::plzcw: {
            u32 words[2];
            for (s32 i = 0; i < 2; ++i) {
                const u32 v = rs.word(i);
                const u32 pattern = (static_cast<s32>(v) < 0) ? ~v : v;
                words[i] = (pattern == 0) ? 31u
                                          : static_cast<u32>(std::countl_zero(pattern)) - 1u;
            }
            cpu.write64(rd, static_cast<u64>(words[0]) | (static_cast<u64>(words[1]) << 32));
            break;
        }

        case Op::psubb: {
            u8 out[16];
            for (s32 i = 0; i < 16; ++i) {
                out[i] = static_cast<u8>(byte(rs, i) - byte(rt, i));
            }
            cpu.write128(rd, fromBytes(out));
            break;
        }
        case Op::paddub: {
            u8 out[16];
            for (s32 i = 0; i < 16; ++i) {
                const u32 sum = static_cast<u32>(byte(rs, i)) + byte(rt, i);
                out[i] = static_cast<u8>(sum > 0xFF ? 0xFF : sum);
            }
            cpu.write128(rd, fromBytes(out));
            break;
        }
        case Op::padduw: {
            u32 out[4];
            for (s32 i = 0; i < 4; ++i) {
                const u64 sum = static_cast<u64>(rs.word(i)) + rt.word(i);
                out[i] = (sum > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<u32>(sum);
            }
            cpu.write128(rd, lanes(out[0], out[1], out[2], out[3]));
            break;
        }

        case Op::pextlb: {
            u8 out[16];
            for (s32 i = 0; i < 8; ++i) {
                out[2 * i] = byte(rt, i);
                out[2 * i + 1] = byte(rs, i);
            }
            cpu.write128(rd, fromBytes(out));
            break;
        }
        case Op::pextlh: {
            u16 out[8];
            for (s32 i = 0; i < 4; ++i) {
                out[2 * i] = half(rt, i);
                out[2 * i + 1] = half(rs, i);
            }
            cpu.write128(rd, fromHalves(out));
            break;
        }
        case Op::ppacb: {
            u8 out[16];
            for (s32 i = 0; i < 8; ++i) {
                out[i] = byte(rt, 2 * i);
                out[i + 8] = byte(rs, 2 * i);
            }
            cpu.write128(rd, fromBytes(out));
            break;
        }
        case Op::ppach: {
            u16 out[8];
            for (s32 i = 0; i < 4; ++i) {
                out[i] = half(rt, 2 * i);
                out[i + 4] = half(rs, 2 * i);
            }
            cpu.write128(rd, fromHalves(out));
            break;
        }
        case Op::ppacw:
            cpu.write128(rd, lanes(rt.word(0), rt.word(2), rs.word(0), rs.word(2)));
            break;
        case Op::pexew:
            cpu.write128(rd, lanes(rt.word(2), rt.word(1), rt.word(0), rt.word(3)));
            break;
        case Op::prot3w:
            cpu.write128(rd, lanes(rt.word(1), rt.word(2), rt.word(0), rt.word(3)));
            break;
        case Op::pcpyh: {
            u16 out[8];
            for (s32 i = 0; i < 4; ++i) {
                out[i] = half(rt, 0);
                out[i + 4] = half(rt, 4);
            }
            cpu.write128(rd, fromHalves(out));
            break;
        }

        case Op::beq: branchIf(rs.lo == rt.lo); break;
        case Op::beql: branchLikelyIf(rs.lo == rt.lo); break;
        case Op::bne: branchIf(rs.lo != rt.lo); break;
        case Op::bnel: branchLikelyIf(rs.lo != rt.lo); break;
        case Op::blez: branchIf(signed64(rs) <= 0); break;
        case Op::blezl: branchLikelyIf(signed64(rs) <= 0); break;
        case Op::bgtz: branchIf(signed64(rs) > 0); break;
        case Op::bgtzl: branchLikelyIf(signed64(rs) > 0); break;
        case Op::bltz: branchIf(signed64(rs) < 0); break;
        case Op::bltzl: branchLikelyIf(signed64(rs) < 0); break;
        case Op::bgez: branchIf(signed64(rs) >= 0); break;
        case Op::bgezl: branchLikelyIf(signed64(rs) >= 0); break;
        case Op::bltzal: link(31); branchIf(signed64(rs) < 0); break;
        case Op::bltzall: link(31); branchLikelyIf(signed64(rs) < 0); break;
        case Op::bgezal: link(31); branchIf(signed64(rs) >= 0); break;
        case Op::bgezall: link(31); branchLikelyIf(signed64(rs) >= 0); break;
        case Op::bc1f: branchIf((machine.fpu.fcr31 & Fpu::kCompareBit) == 0); break;
        case Op::bc1fl: branchLikelyIf((machine.fpu.fcr31 & Fpu::kCompareBit) == 0); break;
        case Op::bc1t: branchIf((machine.fpu.fcr31 & Fpu::kCompareBit) != 0); break;
        case Op::bc1tl: branchLikelyIf((machine.fpu.fcr31 & Fpu::kCompareBit) != 0); break;
        case Op::j: jumpTo(((pc + 4) & 0xF0000000u) | (insn.target() << 2)); break;
        case Op::jal:
            link(31);
            jumpTo(((pc + 4) & 0xF0000000u) | (insn.target() << 2));
            break;
        case Op::jr: jumpTo(low(rs)); break;
        case Op::jalr:
            link(rd);
            jumpTo(low(rs));
            break;

        case Op::lb: cpu.write32(insn.rt(), static_cast<u32>(static_cast<s8>(memory.read8(address)))); break;
        case Op::lbu: cpu.write32(insn.rt(), memory.read8(address)); break;
        case Op::lh: cpu.write32(insn.rt(), static_cast<u32>(static_cast<s16>(memory.read16(address)))); break;
        case Op::lhu: cpu.write32(insn.rt(), memory.read16(address)); break;
        case Op::lw: cpu.write32(insn.rt(), memory.read32(address)); break;
        case Op::lwu: cpu.write64(insn.rt(), memory.read32(address)); break;
        case Op::ld: cpu.write64(insn.rt(), memory.read64(address)); break;
        case Op::lq:
            cpu.write128(insn.rt(),
                     Reg128{memory.read64(address & ~15u), memory.read64((address & ~15u) + 8)});
            break;
        case Op::lwc1: machine.fpu.fpr[insn.rt()] = memory.read32(address); break;
        case Op::lqc2:
            machine.vu0.vf[insn.rt()] =
                Reg128{memory.read64(address & ~15u), memory.read64((address & ~15u) + 8)};
            break;

        case Op::sb: memory.write8(address, static_cast<u8>(low(rt))); break;
        case Op::sh: memory.write16(address, static_cast<u16>(low(rt))); break;
        case Op::sw: memory.write32(address, low(rt)); break;
        case Op::sd: memory.write64(address, rt.lo); break;
        case Op::sq:
            memory.write64(address & ~15u, rt.lo);
            memory.write64((address & ~15u) + 8, rt.hi);
            break;
        case Op::swc1: memory.write32(address, machine.fpu.fpr[insn.rt()]); break;
        case Op::sqc2:
            memory.write64(address & ~15u, machine.vu0.vf[insn.rt()].lo);
            memory.write64((address & ~15u) + 8, machine.vu0.vf[insn.rt()].hi);
            break;

        case Op::lwl: {
            const u32 shift = 8u * (3u - (address & 3u));
            const u32 word = memory.read32(address & ~3u);
            const u32 keep = (shift == 0) ? 0u : (low(rt) & ((1u << shift) - 1u));
            cpu.write32(insn.rt(), (word << shift) | keep);
            break;
        }
        case Op::lwr: {
            const u32 shift = 8u * (address & 3u);
            const u32 word = memory.read32(address & ~3u);
            const u32 keep = (shift == 0) ? 0u : (low(rt) & ~((1u << (32 - shift)) - 1u));
            cpu.write32(insn.rt(), (word >> shift) | keep);
            break;
        }
        case Op::ldl: {
            const u32 shift = 8u * (7u - (address & 7u));
            const u64 value = memory.read64(address & ~7u);
            const u64 keep = (shift == 0) ? 0ull : (rt.lo & ((1ull << shift) - 1ull));
            cpu.write64(insn.rt(), (value << shift) | keep);
            break;
        }
        case Op::ldr: {
            const u32 shift = 8u * (address & 7u);
            const u64 value = memory.read64(address & ~7u);
            const u64 keep = (shift == 0) ? 0ull : (rt.lo & ~((1ull << (64 - shift)) - 1ull));
            cpu.write64(insn.rt(), (value >> shift) | keep);
            break;
        }
        case Op::swl: {
            const u32 bytes = (address & 3u) + 1u;
            const u32 shift = 8u * (4u - bytes);
            const u32 word = memory.read32(address & ~3u);
            const u32 mask = (bytes == 4) ? 0xFFFFFFFFu : ((1u << (8 * bytes)) - 1u);
            memory.write32(address & ~3u, (word & ~mask) | ((low(rt) >> shift) & mask));
            break;
        }
        case Op::swr: {
            const u32 shift = 8u * (address & 3u);
            const u32 word = memory.read32(address & ~3u);
            const u32 mask = (shift == 0) ? 0xFFFFFFFFu : ~((1u << shift) - 1u);
            memory.write32(address & ~3u, (word & ~mask) | ((low(rt) << shift) & mask));
            break;
        }
        case Op::sdl: {
            const u32 bytes = (address & 7u) + 1u;
            const u32 shift = 8u * (8u - bytes);
            const u64 value = memory.read64(address & ~7u);
            const u64 mask = (bytes == 8) ? ~0ull : ((1ull << (8 * bytes)) - 1ull);
            memory.write64(address & ~7u, (value & ~mask) | ((rt.lo >> shift) & mask));
            break;
        }
        case Op::sdr: {
            const u32 shift = 8u * (address & 7u);
            const u64 value = memory.read64(address & ~7u);
            const u64 mask = (shift == 0) ? ~0ull : ~((1ull << shift) - 1ull);
            memory.write64(address & ~7u, (value & ~mask) | ((rt.lo << shift) & mask));
            break;
        }

        case Op::add_s: case Op::sub_s: case Op::mul_s: case Op::div_s:
        case Op::sqrt_s: case Op::abs_s: case Op::neg_s: case Op::mov_s:
        case Op::cvt_w_s: case Op::cvt_s_w:
        case Op::c_eq_s: case Op::c_lt_s: case Op::c_le_s:
        case Op::adda_s: case Op::suba_s: case Op::mula_s:
        case Op::madda_s: case Op::msuba_s: case Op::madd_s: case Op::msub_s:
            if (!mFpu.execute(slot.op, insn)) {
                return Status::Unimplemented;
            }
            break;

        case Op::mfhi: cpu.write64(rd, cpu.hi.lo); break;
        case Op::mflo: cpu.write64(rd, cpu.lo.lo); break;
        case Op::mfhi1: cpu.write64(rd, cpu.hi.hi); break;
        case Op::mflo1: cpu.write64(rd, cpu.lo.hi); break;
        case Op::pmfhi: cpu.write128(rd, cpu.hi); break;
        case Op::pmflo: cpu.write128(rd, cpu.lo); break;
        case Op::pmfhl_lw:
            cpu.write128(rd,
                     lanes(cpu.lo.word(0), cpu.hi.word(0), cpu.lo.word(2), cpu.hi.word(2)));
            break;
        case Op::pmthi: cpu.hi = rs; break;
        case Op::pmtlo: cpu.lo = rs; break;
        case Op::mfsa: cpu.write64(rd, cpu.sa); break;
        case Op::mtsa: cpu.sa = low(rs); break;
        case Op::mtsab: cpu.sa = ((low(rs) & 0xF) ^ (insn.imm() & 0xF)) * 8; break;
        case Op::mtsah: cpu.sa = ((low(rs) & 0x7) ^ (insn.imm() & 0x7)) * 16; break;

        case Op::mfc0: cpu.write32(insn.rt(), cpu.cop0[insn.rd()]); break;
        case Op::mtc0: cpu.cop0[insn.rd()] = low(rt); break;
        case Op::di: cpu.cop0[cop0::kStatus] &= ~(1u << 16); break;
        case Op::ei: cpu.cop0[cop0::kStatus] |= (1u << 16); break;
        case Op::bc0f: branchIf(!cpu.cop0condition); break;
        case Op::bc0fl: branchIf(!cpu.cop0condition); break;
        case Op::bc0t: branchIf(cpu.cop0condition); break;
        case Op::bc0tl: branchIf(cpu.cop0condition); break;
        case Op::syscall: raiseException(cpu, pc, cop0::kSystemCall); break;
        case Op::break_: raiseException(cpu, pc, cop0::kBreakpoint); break;
        case Op::eret: returnFromException(cpu); break;

        // no architectural state this models
        case Op::sync_p:
        case Op::cache:
        case Op::pref:
        case Op::tlbwi:
            break;

        case Op::mfc1: cpu.write32(insn.rt(), machine.fpu.fpr[insn.fs()]); break;
        case Op::mtc1: machine.fpu.fpr[insn.fs()] = low(rt); break;
        case Op::cfc1:
            if (insn.fs() != 31) {
                return Status::Unimplemented;
            }
            cpu.write32(insn.rt(), machine.fpu.fcr31);
            break;
        case Op::ctc1:
            if (insn.fs() != 31) {
                return Status::Unimplemented;
            }
            machine.fpu.fcr31 = low(rt);
            break;

        case Op::qmfc2_ni: cpu.write128(insn.rt(), machine.vu0.readVf(insn.vs())); break;
        case Op::qmtc2_ni: machine.vu0.vf[insn.vs()] = rt; break;
        case Op::cfc2_ni: cpu.write32(insn.rt(), machine.vu0.vi[insn.rd()]); break;
        case Op::ctc2_ni:
            // R holds a 23-bit mantissa and reads back with the exponent fixed
            machine.vu0.vi[insn.rd()] = (insn.rd() == Vu0::kR)
                                         ? (0x3F800000u | (low(rt) & 0x7FFFFFu))
                                         : low(rt);
            break;

        default:
            if (!mVu0.execute(slot.op, insn)) {
                return Status::Unimplemented;
            }
            break;
    }
    return Status::Executed;
}

}  // namespace uee
