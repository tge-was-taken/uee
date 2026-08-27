#pragma once

#include "fpu.h"
#include "memory.h"
#include "r5900.h"
#include "types.h"
#include "vu0.h"

#include <bit>

namespace uee {

namespace reg {
constexpr u32 zero = 0, at = 1, v0 = 2, v1 = 3;
constexpr u32 a0 = 4, a1 = 5, a2 = 6, a3 = 7;
constexpr u32 t0 = 8, t1 = 9, t2 = 10, t3 = 11, t4 = 12, t5 = 13, t6 = 14, t7 = 15;
constexpr u32 s0 = 16, s1 = 17, s2 = 18, s3 = 19, s4 = 20, s5 = 21, s6 = 22, s7 = 23;
constexpr u32 t8 = 24, t9 = 25, k0 = 26, k1 = 27;
constexpr u32 gp = 28, sp = 29, fp = 30, ra = 31;
}  // namespace reg

// The R5900 puts the low word in rd as well as in LO.
inline void setMultiplyResult(R5900& cpu, u32 rd, u64 product, bool pipeline1) {
    const u32 lowWord = static_cast<u32>(product);
    const u32 highWord = static_cast<u32>(product >> 32);
    const u64 loValue = static_cast<u64>(static_cast<s64>(static_cast<s32>(lowWord)));
    const u64 hiValue = static_cast<u64>(static_cast<s64>(static_cast<s32>(highWord)));
    if (pipeline1) {
        cpu.lo.hi = loValue;
        cpu.hi.hi = hiValue;
    } else {
        cpu.lo.lo = loValue;
        cpu.hi.lo = hiValue;
    }
    cpu.write32(rd, lowWord);
}

inline void setDivideResult(R5900& cpu, u32 quotient, u32 remainder, bool pipeline1) {
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

// HI and LO each hold a sign-extended 32-bit half of the accumulator.
constexpr u64 multiplyAccumulator(const R5900& cpu) {
    return (static_cast<u64>(cpu.hi.word(0)) << 32) | cpu.lo.word(0);
}

// MIPS leaves signed division by zero and INT_MIN / -1 undefined; the reference
// produces these, so they are reproduced rather than guessed at.
inline void divideSigned(R5900& cpu, s32 a, s32 b, bool pipeline1) {
    if (b == 0) {
        setDivideResult(cpu, a < 0 ? 1u : 0xFFFFFFFFu, static_cast<u32>(a), pipeline1);
    } else if (b == -1 && a == static_cast<s32>(0x80000000)) {
        setDivideResult(cpu, 0x80000000u, 0, pipeline1);
    } else {
        setDivideResult(cpu, static_cast<u32>(a / b), static_cast<u32>(a % b), pipeline1);
    }
}

inline void divideUnsigned(R5900& cpu, u32 a, u32 b, bool pipeline1) {
    if (b == 0) {
        setDivideResult(cpu, 0xFFFFFFFFu, a, pipeline1);
    } else {
        setDivideResult(cpu, a / b, a % b, pipeline1);
    }
}

inline u32 effectiveAddress(const R5900& cpu, u32 base, s32 offset) {
    return low(cpu.gpr[base]) + static_cast<u32>(offset);
}

inline void sll(R5900& cpu, u32 rd, u32 rt, u32 sa) {
    cpu.write32(rd, low(cpu.gpr[rt]) << sa);
}
inline void srl(R5900& cpu, u32 rd, u32 rt, u32 sa) {
    cpu.write32(rd, low(cpu.gpr[rt]) >> sa);
}
inline void sra(R5900& cpu, u32 rd, u32 rt, u32 sa) {
    cpu.write32(rd, static_cast<u32>(static_cast<s32>(low(cpu.gpr[rt])) >> sa));
}
inline void dsll(R5900& cpu, u32 rd, u32 rt, u32 sa) {
    cpu.write64(rd, cpu.gpr[rt].lo << sa);
}
inline void dsrl(R5900& cpu, u32 rd, u32 rt, u32 sa) {
    cpu.write64(rd, cpu.gpr[rt].lo >> sa);
}
inline void dsra(R5900& cpu, u32 rd, u32 rt, u32 sa) {
    cpu.write64(rd, static_cast<u64>(signed64(cpu.gpr[rt]) >> sa));
}
inline void dsll32(R5900& cpu, u32 rd, u32 rt, u32 sa) {
    cpu.write64(rd, cpu.gpr[rt].lo << (sa + 32));
}
inline void dsrl32(R5900& cpu, u32 rd, u32 rt, u32 sa) {
    cpu.write64(rd, cpu.gpr[rt].lo >> (sa + 32));
}
inline void dsra32(R5900& cpu, u32 rd, u32 rt, u32 sa) {
    cpu.write64(rd, static_cast<u64>(signed64(cpu.gpr[rt]) >> (sa + 32)));
}

inline void sllv(R5900& cpu, u32 rd, u32 rt, u32 rs) {
    cpu.write32(rd, low(cpu.gpr[rt]) << (low(cpu.gpr[rs]) & 31));
}
inline void srlv(R5900& cpu, u32 rd, u32 rt, u32 rs) {
    cpu.write32(rd, low(cpu.gpr[rt]) >> (low(cpu.gpr[rs]) & 31));
}
inline void srav(R5900& cpu, u32 rd, u32 rt, u32 rs) {
    cpu.write32(rd,
                static_cast<u32>(static_cast<s32>(low(cpu.gpr[rt])) >> (low(cpu.gpr[rs]) & 31)));
}
inline void dsllv(R5900& cpu, u32 rd, u32 rt, u32 rs) {
    cpu.write64(rd, cpu.gpr[rt].lo << (low(cpu.gpr[rs]) & 63));
}
inline void dsrlv(R5900& cpu, u32 rd, u32 rt, u32 rs) {
    cpu.write64(rd, cpu.gpr[rt].lo >> (low(cpu.gpr[rs]) & 63));
}
inline void dsrav(R5900& cpu, u32 rd, u32 rt, u32 rs) {
    cpu.write64(rd, static_cast<u64>(signed64(cpu.gpr[rt]) >> (low(cpu.gpr[rs]) & 63)));
}

// The trapping forms are not modelled, so add is addu and sub is subu.
inline void addu(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    cpu.write32(rd, low(cpu.gpr[rs]) + low(cpu.gpr[rt]));
}
inline void subu(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    cpu.write32(rd, low(cpu.gpr[rs]) - low(cpu.gpr[rt]));
}
inline void daddu(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    cpu.write64(rd, cpu.gpr[rs].lo + cpu.gpr[rt].lo);
}
inline void dsubu(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    cpu.write64(rd, cpu.gpr[rs].lo - cpu.gpr[rt].lo);
}
inline void and_(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    cpu.write64(rd, cpu.gpr[rs].lo & cpu.gpr[rt].lo);
}
inline void or_(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    cpu.write64(rd, cpu.gpr[rs].lo | cpu.gpr[rt].lo);
}
inline void xor_(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    cpu.write64(rd, cpu.gpr[rs].lo ^ cpu.gpr[rt].lo);
}
inline void nor(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    cpu.write64(rd, ~(cpu.gpr[rs].lo | cpu.gpr[rt].lo));
}
inline void slt(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    cpu.write64(rd, signed64(cpu.gpr[rs]) < signed64(cpu.gpr[rt]) ? 1 : 0);
}
inline void sltu(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    cpu.write64(rd, cpu.gpr[rs].lo < cpu.gpr[rt].lo ? 1 : 0);
}

inline void addiu(R5900& cpu, u32 rt, u32 rs, s32 imm) {
    cpu.write32(rt, low(cpu.gpr[rs]) + static_cast<u32>(imm));
}
inline void daddiu(R5900& cpu, u32 rt, u32 rs, s32 imm) {
    cpu.write64(rt, cpu.gpr[rs].lo + static_cast<u64>(static_cast<s64>(imm)));
}
inline void andi(R5900& cpu, u32 rt, u32 rs, u32 imm) {
    cpu.write64(rt, cpu.gpr[rs].lo & imm);
}
inline void ori(R5900& cpu, u32 rt, u32 rs, u32 imm) {
    cpu.write64(rt, cpu.gpr[rs].lo | imm);
}
inline void xori(R5900& cpu, u32 rt, u32 rs, u32 imm) {
    cpu.write64(rt, cpu.gpr[rs].lo ^ imm);
}
inline void slti(R5900& cpu, u32 rt, u32 rs, s32 imm) {
    cpu.write64(rt, signed64(cpu.gpr[rs]) < imm ? 1 : 0);
}
inline void sltiu(R5900& cpu, u32 rt, u32 rs, s32 imm) {
    cpu.write64(rt, cpu.gpr[rs].lo < static_cast<u64>(static_cast<s64>(imm)) ? 1 : 0);
}
inline void lui(R5900& cpu, u32 rt, u32 imm) {
    cpu.write32(rt, imm << 16);
}

inline void movz(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    if (cpu.gpr[rt].lo == 0) {
        cpu.write64(rd, cpu.gpr[rs].lo);
    }
}
inline void movn(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    if (cpu.gpr[rt].lo != 0) {
        cpu.write64(rd, cpu.gpr[rs].lo);
    }
}

inline void pand(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    const Reg128 a = cpu.gpr[rs], b = cpu.gpr[rt];
    cpu.write128(rd, Reg128{a.lo & b.lo, a.hi & b.hi});
}
inline void por(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    const Reg128 a = cpu.gpr[rs], b = cpu.gpr[rt];
    cpu.write128(rd, Reg128{a.lo | b.lo, a.hi | b.hi});
}
inline void pxor(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    const Reg128 a = cpu.gpr[rs], b = cpu.gpr[rt];
    cpu.write128(rd, Reg128{a.lo ^ b.lo, a.hi ^ b.hi});
}
inline void pnor(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    const Reg128 a = cpu.gpr[rs], b = cpu.gpr[rt];
    cpu.write128(rd, Reg128{~(a.lo | b.lo), ~(a.hi | b.hi)});
}
inline void pcpyld(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    cpu.write128(rd, Reg128{cpu.gpr[rt].lo, cpu.gpr[rs].lo});
}
inline void pcpyud(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    cpu.write128(rd, Reg128{cpu.gpr[rs].hi, cpu.gpr[rt].hi});
}
inline void pextlw(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    const Reg128 a = cpu.gpr[rs], b = cpu.gpr[rt];
    cpu.write128(rd, lanes(b.word(0), a.word(0), b.word(1), a.word(1)));
}
inline void pextuw(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    const Reg128 a = cpu.gpr[rs], b = cpu.gpr[rt];
    cpu.write128(rd, lanes(b.word(2), a.word(2), b.word(3), a.word(3)));
}
inline void paddw(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    const Reg128 a = cpu.gpr[rs], b = cpu.gpr[rt];
    cpu.write128(rd, lanes(a.word(0) + b.word(0), a.word(1) + b.word(1), a.word(2) + b.word(2),
                           a.word(3) + b.word(3)));
}
inline void psubw(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    const Reg128 a = cpu.gpr[rs], b = cpu.gpr[rt];
    cpu.write128(rd, lanes(a.word(0) - b.word(0), a.word(1) - b.word(1), a.word(2) - b.word(2),
                           a.word(3) - b.word(3)));
}
inline void psubb(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    const Reg128 a = cpu.gpr[rs], b = cpu.gpr[rt];
    u8 out[16];
    for (s32 i = 0; i < 16; ++i) {
        out[i] = static_cast<u8>(byte(a, i) - byte(b, i));
    }
    cpu.write128(rd, fromBytes(out));
}
inline void paddub(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    const Reg128 a = cpu.gpr[rs], b = cpu.gpr[rt];
    u8 out[16];
    for (s32 i = 0; i < 16; ++i) {
        const u32 sum = static_cast<u32>(byte(a, i)) + byte(b, i);
        out[i] = static_cast<u8>(sum > 0xFF ? 0xFF : sum);
    }
    cpu.write128(rd, fromBytes(out));
}
inline void padduw(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    const Reg128 a = cpu.gpr[rs], b = cpu.gpr[rt];
    u32 out[4];
    for (s32 i = 0; i < 4; ++i) {
        const u64 sum = static_cast<u64>(a.word(i)) + b.word(i);
        out[i] = (sum > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<u32>(sum);
    }
    cpu.write128(rd, lanes(out[0], out[1], out[2], out[3]));
}
inline void pextlb(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    const Reg128 a = cpu.gpr[rs], b = cpu.gpr[rt];
    u8 out[16];
    for (s32 i = 0; i < 8; ++i) {
        out[2 * i] = byte(b, i);
        out[2 * i + 1] = byte(a, i);
    }
    cpu.write128(rd, fromBytes(out));
}
inline void pextlh(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    const Reg128 a = cpu.gpr[rs], b = cpu.gpr[rt];
    u16 out[8];
    for (s32 i = 0; i < 4; ++i) {
        out[2 * i] = half(b, i);
        out[2 * i + 1] = half(a, i);
    }
    cpu.write128(rd, fromHalves(out));
}
inline void ppacb(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    const Reg128 a = cpu.gpr[rs], b = cpu.gpr[rt];
    u8 out[16];
    for (s32 i = 0; i < 8; ++i) {
        out[i] = byte(b, 2 * i);
        out[i + 8] = byte(a, 2 * i);
    }
    cpu.write128(rd, fromBytes(out));
}
inline void ppach(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    const Reg128 a = cpu.gpr[rs], b = cpu.gpr[rt];
    u16 out[8];
    for (s32 i = 0; i < 4; ++i) {
        out[i] = half(b, 2 * i);
        out[i + 4] = half(a, 2 * i);
    }
    cpu.write128(rd, fromHalves(out));
}
inline void ppacw(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    const Reg128 a = cpu.gpr[rs], b = cpu.gpr[rt];
    cpu.write128(rd, lanes(b.word(0), b.word(2), a.word(0), a.word(2)));
}
inline void pexew(R5900& cpu, u32 rd, u32 rt) {
    const Reg128 b = cpu.gpr[rt];
    cpu.write128(rd, lanes(b.word(2), b.word(1), b.word(0), b.word(3)));
}
inline void prot3w(R5900& cpu, u32 rd, u32 rt) {
    const Reg128 b = cpu.gpr[rt];
    cpu.write128(rd, lanes(b.word(1), b.word(2), b.word(0), b.word(3)));
}
inline void pcpyh(R5900& cpu, u32 rd, u32 rt) {
    const Reg128 b = cpu.gpr[rt];
    u16 out[8];
    for (s32 i = 0; i < 4; ++i) {
        out[i] = half(b, 0);
        out[i + 4] = half(b, 4);
    }
    cpu.write128(rd, fromHalves(out));
}
inline void plzcw(R5900& cpu, u32 rd, u32 rs) {
    const Reg128 a = cpu.gpr[rs];
    u32 words[2];
    for (s32 i = 0; i < 2; ++i) {
        const u32 v = a.word(i);
        const u32 pattern = (static_cast<s32>(v) < 0) ? ~v : v;
        words[i] = (pattern == 0) ? 31u : static_cast<u32>(std::countl_zero(pattern)) - 1u;
    }
    cpu.write64(rd, static_cast<u64>(words[0]) | (static_cast<u64>(words[1]) << 32));
}

inline void mult(R5900& cpu, u32 rd, u32 rs, u32 rt, bool pipeline1) {
    setMultiplyResult(cpu, rd,
                      static_cast<u64>(static_cast<s64>(static_cast<s32>(low(cpu.gpr[rs]))) *
                                       static_cast<s64>(static_cast<s32>(low(cpu.gpr[rt])))),
                      pipeline1);
}
inline void multu(R5900& cpu, u32 rd, u32 rs, u32 rt, bool pipeline1) {
    setMultiplyResult(cpu, rd,
                      static_cast<u64>(low(cpu.gpr[rs])) * static_cast<u64>(low(cpu.gpr[rt])),
                      pipeline1);
}
inline void madd(R5900& cpu, u32 rd, u32 rs, u32 rt, bool pipeline1) {
    setMultiplyResult(cpu, rd,
                      multiplyAccumulator(cpu) +
                          static_cast<u64>(static_cast<s64>(static_cast<s32>(low(cpu.gpr[rs]))) *
                                           static_cast<s64>(static_cast<s32>(low(cpu.gpr[rt])))),
                      pipeline1);
}
inline void div(R5900& cpu, u32 rs, u32 rt, bool pipeline1) {
    divideSigned(cpu, static_cast<s32>(low(cpu.gpr[rs])), static_cast<s32>(low(cpu.gpr[rt])),
                 pipeline1);
}
inline void divu(R5900& cpu, u32 rs, u32 rt, bool pipeline1) {
    divideUnsigned(cpu, low(cpu.gpr[rs]), low(cpu.gpr[rt]), pipeline1);
}

inline void mthi(R5900& cpu, u32 rs, bool pipeline1) {
    (pipeline1 ? cpu.hi.hi : cpu.hi.lo) = cpu.gpr[rs].lo;
}
inline void mtlo(R5900& cpu, u32 rs, bool pipeline1) {
    (pipeline1 ? cpu.lo.hi : cpu.lo.lo) = cpu.gpr[rs].lo;
}
inline void mfhi(R5900& cpu, u32 rd, bool pipeline1) {
    cpu.write64(rd, pipeline1 ? cpu.hi.hi : cpu.hi.lo);
}
inline void mflo(R5900& cpu, u32 rd, bool pipeline1) {
    cpu.write64(rd, pipeline1 ? cpu.lo.hi : cpu.lo.lo);
}
inline void pmfhi(R5900& cpu, u32 rd) {
    cpu.write128(rd, cpu.hi);
}
inline void pmflo(R5900& cpu, u32 rd) {
    cpu.write128(rd, cpu.lo);
}
inline void pmfhl_lw(R5900& cpu, u32 rd) {
    cpu.write128(rd, lanes(cpu.lo.word(0), cpu.hi.word(0), cpu.lo.word(2), cpu.hi.word(2)));
}
inline void pmthi(R5900& cpu, u32 rs) {
    cpu.hi = cpu.gpr[rs];
}
inline void pmtlo(R5900& cpu, u32 rs) {
    cpu.lo = cpu.gpr[rs];
}

inline void mfsa(R5900& cpu, u32 rd) {
    cpu.write64(rd, cpu.sa);
}
inline void mtsa(R5900& cpu, u32 rs) {
    cpu.sa = low(cpu.gpr[rs]);
}
inline void mtsab(R5900& cpu, u32 rs, u32 imm) {
    cpu.sa = ((low(cpu.gpr[rs]) & 0xF) ^ (imm & 0xF)) * 8;
}
inline void mtsah(R5900& cpu, u32 rs, u32 imm) {
    cpu.sa = ((low(cpu.gpr[rs]) & 0x7) ^ (imm & 0x7)) * 16;
}

inline void mfc0(R5900& cpu, u32 rt, u32 rd) {
    cpu.write32(rt, cpu.cop0[rd]);
}
inline void mtc0(R5900& cpu, u32 rt, u32 rd) {
    cpu.cop0[rd] = low(cpu.gpr[rt]);
}
inline void di(R5900& cpu) {
    cpu.cop0[cop0::kStatus] &= ~(1u << 16);
}
inline void ei(R5900& cpu) {
    cpu.cop0[cop0::kStatus] |= (1u << 16);
}

// EPC keeps the address of the first exception, so a fault inside a handler
// does not lose where the handler was entered from.
inline void raiseException(R5900& cpu, u32 pc, u32 code) {
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

inline void returnFromException(R5900& cpu) {
    u32& status = cpu.cop0[cop0::kStatus];
    const bool fromError = (status & cop0::kErrorLevel) != 0;
    cpu.pc = cpu.cop0[fromError ? cop0::kErrorEpc : cop0::kEpc];
    status &= ~(fromError ? cop0::kErrorLevel : cop0::kExceptionLevel);
}

inline void mfc1(R5900& cpu, const Fpu& fpu, u32 rt, u32 fs) {
    cpu.write32(rt, fpu.fpr[fs]);
}
inline void mtc1(R5900& cpu, Fpu& fpu, u32 rt, u32 fs) {
    fpu.fpr[fs] = low(cpu.gpr[rt]);
}
inline void cfc1(R5900& cpu, const Fpu& fpu, u32 rt) {
    cpu.write32(rt, fpu.fcr31);
}
inline void ctc1(R5900& cpu, Fpu& fpu, u32 rt) {
    fpu.fcr31 = low(cpu.gpr[rt]);
}

inline void qmfc2(R5900& cpu, const Vu0& vu0, u32 rt, u32 vs) {
    cpu.write128(rt, vu0.readVf(vs));
}
inline void qmtc2(R5900& cpu, Vu0& vu0, u32 rt, u32 vs) {
    vu0.vf[vs] = cpu.gpr[rt];
}
inline void cfc2(R5900& cpu, const Vu0& vu0, u32 rt, u32 rd) {
    cpu.write32(rt, vu0.vi[rd]);
}
inline void ctc2(R5900& cpu, Vu0& vu0, u32 rt, u32 rd) {
    // R holds a 23-bit mantissa and reads back with the exponent fixed
    vu0.vi[rd] =
        (rd == Vu0::kR) ? (0x3F800000u | (low(cpu.gpr[rt]) & 0x7FFFFFu)) : low(cpu.gpr[rt]);
}

inline void lb(R5900& cpu, Memory& memory, u32 rt, u32 base, s32 offset) {
    cpu.write32(rt, static_cast<u32>(static_cast<s8>(
                        memory.read8(effectiveAddress(cpu, base, offset)))));
}
inline void lbu(R5900& cpu, Memory& memory, u32 rt, u32 base, s32 offset) {
    cpu.write32(rt, memory.read8(effectiveAddress(cpu, base, offset)));
}
inline void lh(R5900& cpu, Memory& memory, u32 rt, u32 base, s32 offset) {
    cpu.write32(rt, static_cast<u32>(static_cast<s16>(
                        memory.read16(effectiveAddress(cpu, base, offset)))));
}
inline void lhu(R5900& cpu, Memory& memory, u32 rt, u32 base, s32 offset) {
    cpu.write32(rt, memory.read16(effectiveAddress(cpu, base, offset)));
}
inline void lw(R5900& cpu, Memory& memory, u32 rt, u32 base, s32 offset) {
    cpu.write32(rt, memory.read32(effectiveAddress(cpu, base, offset)));
}
inline void lwu(R5900& cpu, Memory& memory, u32 rt, u32 base, s32 offset) {
    cpu.write64(rt, memory.read32(effectiveAddress(cpu, base, offset)));
}
inline void ld(R5900& cpu, Memory& memory, u32 rt, u32 base, s32 offset) {
    cpu.write64(rt, memory.read64(effectiveAddress(cpu, base, offset)));
}
inline void lq(R5900& cpu, Memory& memory, u32 rt, u32 base, s32 offset) {
    const u32 aligned = effectiveAddress(cpu, base, offset) & ~15u;
    cpu.write128(rt, Reg128{memory.read64(aligned), memory.read64(aligned + 8)});
}
inline void lwc1(R5900& cpu, Fpu& fpu, Memory& memory, u32 ft, u32 base, s32 offset) {
    fpu.fpr[ft] = memory.read32(effectiveAddress(cpu, base, offset));
}
inline void lqc2(R5900& cpu, Vu0& vu0, Memory& memory, u32 ft, u32 base, s32 offset) {
    const u32 aligned = effectiveAddress(cpu, base, offset) & ~15u;
    vu0.vf[ft] = Reg128{memory.read64(aligned), memory.read64(aligned + 8)};
}

inline void sb(R5900& cpu, Memory& memory, u32 rt, u32 base, s32 offset) {
    memory.write8(effectiveAddress(cpu, base, offset), static_cast<u8>(low(cpu.gpr[rt])));
}
inline void sh(R5900& cpu, Memory& memory, u32 rt, u32 base, s32 offset) {
    memory.write16(effectiveAddress(cpu, base, offset), static_cast<u16>(low(cpu.gpr[rt])));
}
inline void sw(R5900& cpu, Memory& memory, u32 rt, u32 base, s32 offset) {
    memory.write32(effectiveAddress(cpu, base, offset), low(cpu.gpr[rt]));
}
inline void sd(R5900& cpu, Memory& memory, u32 rt, u32 base, s32 offset) {
    memory.write64(effectiveAddress(cpu, base, offset), cpu.gpr[rt].lo);
}
inline void sq(R5900& cpu, Memory& memory, u32 rt, u32 base, s32 offset) {
    const u32 aligned = effectiveAddress(cpu, base, offset) & ~15u;
    memory.write64(aligned, cpu.gpr[rt].lo);
    memory.write64(aligned + 8, cpu.gpr[rt].hi);
}
inline void swc1(R5900& cpu, const Fpu& fpu, Memory& memory, u32 ft, u32 base, s32 offset) {
    memory.write32(effectiveAddress(cpu, base, offset), fpu.fpr[ft]);
}
inline void sqc2(R5900& cpu, const Vu0& vu0, Memory& memory, u32 ft, u32 base, s32 offset) {
    const u32 aligned = effectiveAddress(cpu, base, offset) & ~15u;
    memory.write64(aligned, vu0.vf[ft].lo);
    memory.write64(aligned + 8, vu0.vf[ft].hi);
}

inline void lwl(R5900& cpu, Memory& memory, u32 rt, u32 base, s32 offset) {
    const u32 address = effectiveAddress(cpu, base, offset);
    const u32 shift = 8u * (3u - (address & 3u));
    const u32 word = memory.read32(address & ~3u);
    const u32 keep = (shift == 0) ? 0u : (low(cpu.gpr[rt]) & ((1u << shift) - 1u));
    cpu.write32(rt, (word << shift) | keep);
}
inline void lwr(R5900& cpu, Memory& memory, u32 rt, u32 base, s32 offset) {
    const u32 address = effectiveAddress(cpu, base, offset);
    const u32 shift = 8u * (address & 3u);
    const u32 word = memory.read32(address & ~3u);
    const u32 keep = (shift == 0) ? 0u : (low(cpu.gpr[rt]) & ~((1u << (32 - shift)) - 1u));
    cpu.write32(rt, (word >> shift) | keep);
}
inline void ldl(R5900& cpu, Memory& memory, u32 rt, u32 base, s32 offset) {
    const u32 address = effectiveAddress(cpu, base, offset);
    const u32 shift = 8u * (7u - (address & 7u));
    const u64 value = memory.read64(address & ~7u);
    const u64 keep = (shift == 0) ? 0ull : (cpu.gpr[rt].lo & ((1ull << shift) - 1ull));
    cpu.write64(rt, (value << shift) | keep);
}
inline void ldr(R5900& cpu, Memory& memory, u32 rt, u32 base, s32 offset) {
    const u32 address = effectiveAddress(cpu, base, offset);
    const u32 shift = 8u * (address & 7u);
    const u64 value = memory.read64(address & ~7u);
    const u64 keep = (shift == 0) ? 0ull : (cpu.gpr[rt].lo & ~((1ull << (64 - shift)) - 1ull));
    cpu.write64(rt, (value >> shift) | keep);
}
inline void swl(R5900& cpu, Memory& memory, u32 rt, u32 base, s32 offset) {
    const u32 address = effectiveAddress(cpu, base, offset);
    const u32 bytes = (address & 3u) + 1u;
    const u32 shift = 8u * (4u - bytes);
    const u32 word = memory.read32(address & ~3u);
    const u32 mask = (bytes == 4) ? 0xFFFFFFFFu : ((1u << (8 * bytes)) - 1u);
    memory.write32(address & ~3u, (word & ~mask) | ((low(cpu.gpr[rt]) >> shift) & mask));
}
inline void swr(R5900& cpu, Memory& memory, u32 rt, u32 base, s32 offset) {
    const u32 address = effectiveAddress(cpu, base, offset);
    const u32 shift = 8u * (address & 3u);
    const u32 word = memory.read32(address & ~3u);
    const u32 mask = (shift == 0) ? 0xFFFFFFFFu : ~((1u << shift) - 1u);
    memory.write32(address & ~3u, (word & ~mask) | ((low(cpu.gpr[rt]) << shift) & mask));
}
inline void sdl(R5900& cpu, Memory& memory, u32 rt, u32 base, s32 offset) {
    const u32 address = effectiveAddress(cpu, base, offset);
    const u32 bytes = (address & 7u) + 1u;
    const u32 shift = 8u * (8u - bytes);
    const u64 value = memory.read64(address & ~7u);
    const u64 mask = (bytes == 8) ? ~0ull : ((1ull << (8 * bytes)) - 1ull);
    memory.write64(address & ~7u, (value & ~mask) | ((cpu.gpr[rt].lo >> shift) & mask));
}
inline void sdr(R5900& cpu, Memory& memory, u32 rt, u32 base, s32 offset) {
    const u32 address = effectiveAddress(cpu, base, offset);
    const u32 shift = 8u * (address & 7u);
    const u64 value = memory.read64(address & ~7u);
    const u64 mask = (shift == 0) ? ~0ull : ~((1ull << shift) - 1ull);
    memory.write64(address & ~7u, (value & ~mask) | ((cpu.gpr[rt].lo << shift) & mask));
}

// The transfer stays with the caller so the interpreter can set pc while
// emitted code branches on the same predicate.
constexpr bool beqTaken(const R5900& cpu, u32 rs, u32 rt) {
    return cpu.gpr[rs].lo == cpu.gpr[rt].lo;
}
constexpr bool bneTaken(const R5900& cpu, u32 rs, u32 rt) {
    return cpu.gpr[rs].lo != cpu.gpr[rt].lo;
}
constexpr bool blezTaken(const R5900& cpu, u32 rs) {
    return signed64(cpu.gpr[rs]) <= 0;
}
constexpr bool bgtzTaken(const R5900& cpu, u32 rs) {
    return signed64(cpu.gpr[rs]) > 0;
}
constexpr bool bltzTaken(const R5900& cpu, u32 rs) {
    return signed64(cpu.gpr[rs]) < 0;
}
constexpr bool bgezTaken(const R5900& cpu, u32 rs) {
    return signed64(cpu.gpr[rs]) >= 0;
}
constexpr bool bc1tTaken(const Fpu& fpu) {
    return (fpu.fcr31 & Fpu::kCompareBit) != 0;
}

}  // namespace uee
