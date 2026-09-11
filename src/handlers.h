#pragma once

#include "fpu.h"
#include "fpu_handlers.h"
#include "instruction.h"
#include "ee_mmio.h"
#include "guest_replacement.h"
#include "memory.h"
#include "r5900.h"
#include "types.h"
#include "ee_context.h"
#include "vu.h"
#include "vu_micro.h"
#include "vu_handlers.h"

#include <bit>
#include <limits>
#include <utility>

namespace uee {

// The MMI unit is the same operation repeated across a register at one of three
// widths, so the lane walk is written once and the op supplies the arithmetic.
template <typename F>
inline Reg128 eachByte(const Reg128& a, const Reg128& b, F combine) {
    u8 out[16];
    for (s32 i = 0; i < 16; ++i) {
        out[i] = combine(byte(a, i), byte(b, i));
    }
    return fromBytes(out);
}
template <typename F>
inline Reg128 eachHalf(const Reg128& a, const Reg128& b, F combine) {
    u16 out[8];
    for (s32 i = 0; i < 8; ++i) {
        out[i] = combine(half(a, i), half(b, i));
    }
    return fromHalves(out);
}
template <typename F>
inline Reg128 eachWord(const Reg128& a, const Reg128& b, F combine) {
    u32 out[4];
    for (s32 i = 0; i < 4; ++i) {
        out[i] = combine(a.word(i), b.word(i));
    }
    return lanes(out[0], out[1], out[2], out[3]);
}

template <typename T>
constexpr T saturate(s64 value) {
    constexpr s64 low = std::numeric_limits<T>::min();
    constexpr s64 high = std::numeric_limits<T>::max();
    return static_cast<T>(value < low ? low : (value > high ? high : value));
}

constexpr u8 mask8(bool set) { return set ? 0xFFu : 0u; }
constexpr u16 mask16(bool set) { return set ? 0xFFFFu : 0u; }
constexpr u32 mask32(bool set) { return set ? 0xFFFFFFFFu : 0u; }

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

// HI and LO each hold a sign-extended 32-bit half of the accumulator, and the
// second pipeline keeps its own pair in the upper doubleword.
constexpr u64 multiplyAccumulator(const R5900& cpu, bool pipeline1) {
    const s32 lane = pipeline1 ? 2 : 0;
    return (static_cast<u64>(cpu.hi.word(lane)) << 32) | cpu.lo.word(lane);
}

struct Quotient {
    u32 quotient;
    u32 remainder;
};

// MIPS leaves signed division by zero and INT_MIN / -1 undefined; these are
// what the hardware produces rather than a guess.
constexpr Quotient divideSigned(s32 a, s32 b) {
    if (b == 0) {
        return {a < 0 ? 1u : 0xFFFFFFFFu, static_cast<u32>(a)};
    }
    if (b == -1 && a == static_cast<s32>(0x80000000)) {
        return {0x80000000u, 0};
    }
    return {static_cast<u32>(a / b), static_cast<u32>(a % b)};
}

constexpr Quotient divideUnsigned(u32 a, u32 b) {
    return (b == 0) ? Quotient{0xFFFFFFFFu, a} : Quotient{a / b, a % b};
}

inline void divideSigned(R5900& cpu, s32 a, s32 b, bool pipeline1) {
    const Quotient result = divideSigned(a, b);
    setDivideResult(cpu, result.quotient, result.remainder, pipeline1);
}

inline void divideUnsigned(R5900& cpu, u32 a, u32 b, bool pipeline1) {
    const Quotient result = divideUnsigned(a, b);
    setDivideResult(cpu, result.quotient, result.remainder, pipeline1);
}

constexpr u32 resolveEa(const R5900&, FixedAddress base, s32 offset) {
    return base.address + static_cast<u32>(offset);
}

inline u32 resolveEa(const R5900& cpu, u32 base, s32 offset) {
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
// A move has no encoding, so a compiler spells it against register zero.
inline void move(R5900& cpu, u32 rd, u32 rs) {
    cpu.write64(rd, cpu.gpr[rs].lo);
}
inline void moveWord(R5900& cpu, u32 rd, u32 rs) {
    cpu.write32(rd, low(cpu.gpr[rs]));
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


inline void paddb(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    cpu.write128(rd, eachByte(cpu.gpr[rs], cpu.gpr[rt],
                              [](u8 x, u8 y) { return static_cast<u8>(x + y); }));
}
inline void paddh(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    cpu.write128(rd, eachHalf(cpu.gpr[rs], cpu.gpr[rt],
                              [](u16 x, u16 y) { return static_cast<u16>(x + y); }));
}
inline void psubh(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    cpu.write128(rd, eachHalf(cpu.gpr[rs], cpu.gpr[rt],
                              [](u16 x, u16 y) { return static_cast<u16>(x - y); }));
}

inline void paddsb(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    cpu.write128(rd, eachByte(cpu.gpr[rs], cpu.gpr[rt], [](u8 x, u8 y) {
        return static_cast<u8>(saturate<s8>(s64{static_cast<s8>(x)} + static_cast<s8>(y)));
    }));
}
inline void paddsh(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    cpu.write128(rd, eachHalf(cpu.gpr[rs], cpu.gpr[rt], [](u16 x, u16 y) {
        return static_cast<u16>(saturate<s16>(s64{static_cast<s16>(x)} + static_cast<s16>(y)));
    }));
}
inline void paddsw(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    cpu.write128(rd, eachWord(cpu.gpr[rs], cpu.gpr[rt], [](u32 x, u32 y) {
        return static_cast<u32>(saturate<s32>(s64{static_cast<s32>(x)} + static_cast<s32>(y)));
    }));
}
inline void psubsb(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    cpu.write128(rd, eachByte(cpu.gpr[rs], cpu.gpr[rt], [](u8 x, u8 y) {
        return static_cast<u8>(saturate<s8>(s64{static_cast<s8>(x)} - static_cast<s8>(y)));
    }));
}
inline void psubsh(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    cpu.write128(rd, eachHalf(cpu.gpr[rs], cpu.gpr[rt], [](u16 x, u16 y) {
        return static_cast<u16>(saturate<s16>(s64{static_cast<s16>(x)} - static_cast<s16>(y)));
    }));
}
inline void psubsw(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    cpu.write128(rd, eachWord(cpu.gpr[rs], cpu.gpr[rt], [](u32 x, u32 y) {
        return static_cast<u32>(saturate<s32>(s64{static_cast<s32>(x)} - static_cast<s32>(y)));
    }));
}

inline void padduh(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    cpu.write128(rd, eachHalf(cpu.gpr[rs], cpu.gpr[rt], [](u16 x, u16 y) {
        const u32 sum = static_cast<u32>(x) + y;
        return static_cast<u16>(sum > 0xFFFFu ? 0xFFFFu : sum);
    }));
}
inline void psubub(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    cpu.write128(rd, eachByte(cpu.gpr[rs], cpu.gpr[rt],
                              [](u8 x, u8 y) { return static_cast<u8>(x < y ? 0 : x - y); }));
}
inline void psubuh(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    cpu.write128(rd, eachHalf(cpu.gpr[rs], cpu.gpr[rt],
                              [](u16 x, u16 y) { return static_cast<u16>(x < y ? 0 : x - y); }));
}
inline void psubuw(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    cpu.write128(rd, eachWord(cpu.gpr[rs], cpu.gpr[rt],
                              [](u32 x, u32 y) { return x < y ? 0u : x - y; }));
}

inline void pmaxh(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    cpu.write128(rd, eachHalf(cpu.gpr[rs], cpu.gpr[rt], [](u16 x, u16 y) {
        return static_cast<s16>(x) > static_cast<s16>(y) ? x : y;
    }));
}
inline void pminh(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    cpu.write128(rd, eachHalf(cpu.gpr[rs], cpu.gpr[rt], [](u16 x, u16 y) {
        return static_cast<s16>(x) < static_cast<s16>(y) ? x : y;
    }));
}
inline void pmaxw(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    cpu.write128(rd, eachWord(cpu.gpr[rs], cpu.gpr[rt], [](u32 x, u32 y) {
        return static_cast<s32>(x) > static_cast<s32>(y) ? x : y;
    }));
}
inline void pminw(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    cpu.write128(rd, eachWord(cpu.gpr[rs], cpu.gpr[rt], [](u32 x, u32 y) {
        return static_cast<s32>(x) < static_cast<s32>(y) ? x : y;
    }));
}

inline void pabsh(R5900& cpu, u32 rd, u32 rt) {
    cpu.write128(rd, eachHalf(cpu.gpr[rt], cpu.gpr[rt], [](u16 x, u16) {
        const s16 value = static_cast<s16>(x);
        return static_cast<u16>(value < 0 ? saturate<s16>(-s64{value}) : value);
    }));
}
inline void pabsw(R5900& cpu, u32 rd, u32 rt) {
    cpu.write128(rd, eachWord(cpu.gpr[rt], cpu.gpr[rt], [](u32 x, u32) {
        const s32 value = static_cast<s32>(x);
        return static_cast<u32>(value < 0 ? saturate<s32>(-s64{value}) : value);
    }));
}

inline void pceqb(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    cpu.write128(rd, eachByte(cpu.gpr[rs], cpu.gpr[rt],
                              [](u8 x, u8 y) { return mask8(x == y); }));
}
inline void pceqh(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    cpu.write128(rd, eachHalf(cpu.gpr[rs], cpu.gpr[rt],
                              [](u16 x, u16 y) { return mask16(x == y); }));
}
inline void pceqw(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    cpu.write128(rd, eachWord(cpu.gpr[rs], cpu.gpr[rt],
                              [](u32 x, u32 y) { return mask32(x == y); }));
}
inline void pcgtb(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    cpu.write128(rd, eachByte(cpu.gpr[rs], cpu.gpr[rt], [](u8 x, u8 y) {
        return mask8(static_cast<s8>(x) > static_cast<s8>(y));
    }));
}
inline void pcgth(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    cpu.write128(rd, eachHalf(cpu.gpr[rs], cpu.gpr[rt], [](u16 x, u16 y) {
        return mask16(static_cast<s16>(x) > static_cast<s16>(y));
    }));
}
inline void pcgtw(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    cpu.write128(rd, eachWord(cpu.gpr[rs], cpu.gpr[rt], [](u32 x, u32 y) {
        return mask32(static_cast<s32>(x) > static_cast<s32>(y));
    }));
}

inline void psllh(R5900& cpu, u32 rd, u32 rt, u32 sa) {
    const u32 shift = sa & 15u;
    cpu.write128(rd, eachHalf(cpu.gpr[rt], cpu.gpr[rt],
                              [shift](u16 x, u16) { return static_cast<u16>(x << shift); }));
}
inline void psrlh(R5900& cpu, u32 rd, u32 rt, u32 sa) {
    const u32 shift = sa & 15u;
    cpu.write128(rd, eachHalf(cpu.gpr[rt], cpu.gpr[rt],
                              [shift](u16 x, u16) { return static_cast<u16>(x >> shift); }));
}
inline void psrah(R5900& cpu, u32 rd, u32 rt, u32 sa) {
    const u32 shift = sa & 15u;
    cpu.write128(rd, eachHalf(cpu.gpr[rt], cpu.gpr[rt], [shift](u16 x, u16) {
        return static_cast<u16>(static_cast<s16>(x) >> shift);
    }));
}
inline void psllw(R5900& cpu, u32 rd, u32 rt, u32 sa) {
    cpu.write128(rd, eachWord(cpu.gpr[rt], cpu.gpr[rt], [sa](u32 x, u32) { return x << sa; }));
}
inline void psrlw(R5900& cpu, u32 rd, u32 rt, u32 sa) {
    cpu.write128(rd, eachWord(cpu.gpr[rt], cpu.gpr[rt], [sa](u32 x, u32) { return x >> sa; }));
}
inline void psraw(R5900& cpu, u32 rd, u32 rt, u32 sa) {
    cpu.write128(rd, eachWord(cpu.gpr[rt], cpu.gpr[rt], [sa](u32 x, u32) {
        return static_cast<u32>(static_cast<s32>(x) >> sa);
    }));
}

template <typename F>
inline void shiftEachDoubleword(R5900& cpu, u32 rd, u32 rt, u32 rs, F shift) {
    const Reg128 a = cpu.gpr[rt], b = cpu.gpr[rs];
    const u32 first = shift(a.word(0), b.word(0) & 31u);
    const u32 second = shift(a.word(2), b.word(2) & 31u);
    cpu.write128(rd, Reg128{static_cast<u64>(static_cast<s64>(static_cast<s32>(first))),
                            static_cast<u64>(static_cast<s64>(static_cast<s32>(second)))});
}
inline void psllvw(R5900& cpu, u32 rd, u32 rt, u32 rs) {
    shiftEachDoubleword(cpu, rd, rt, rs, [](u32 x, u32 by) { return x << by; });
}
inline void psrlvw(R5900& cpu, u32 rd, u32 rt, u32 rs) {
    shiftEachDoubleword(cpu, rd, rt, rs, [](u32 x, u32 by) { return x >> by; });
}
inline void psravw(R5900& cpu, u32 rd, u32 rt, u32 rs) {
    shiftEachDoubleword(cpu, rd, rt, rs, [](u32 x, u32 by) {
        return static_cast<u32>(static_cast<s32>(x) >> by);
    });
}

// SA counts bytes, so the pair shifts by whole bytes out of a 256-bit value
// with rs above rt.
inline void qfsrv(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    const u32 shift = cpu.sa & 15u;
    u8 out[16];
    for (s32 i = 0; i < 16; ++i) {
        const s32 source = i + static_cast<s32>(shift);
        out[i] = (source < 16) ? byte(cpu.gpr[rt], source) : byte(cpu.gpr[rs], source - 16);
    }
    cpu.write128(rd, fromBytes(out));
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
                      multiplyAccumulator(cpu, pipeline1) +
                          static_cast<u64>(static_cast<s64>(static_cast<s32>(low(cpu.gpr[rs]))) *
                                           static_cast<s64>(static_cast<s32>(low(cpu.gpr[rt])))),
                      pipeline1);
}

// The wide multiplies fill HI and LO a word at a time; the low half of each
// 64-bit product also lands in rd, the same way mult does.
inline void setWideResult(R5900& cpu, u32 rd, const s64 products[2]) {
    for (s32 i = 0; i < 2; ++i) {
        const u32 lowWord = static_cast<u32>(products[i]);
        const u32 highWord = static_cast<u32>(static_cast<u64>(products[i]) >> 32);
        (i == 0 ? cpu.lo.lo : cpu.lo.hi) =
            static_cast<u64>(static_cast<s64>(static_cast<s32>(lowWord)));
        (i == 0 ? cpu.hi.lo : cpu.hi.hi) =
            static_cast<u64>(static_cast<s64>(static_cast<s32>(highWord)));
    }
    cpu.write128(rd, Reg128{static_cast<u64>(products[0]), static_cast<u64>(products[1])});
}

inline void pmultw(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    const Reg128 a = cpu.gpr[rs], b = cpu.gpr[rt];
    const s64 products[2] = {s64{static_cast<s32>(a.word(0))} * static_cast<s32>(b.word(0)),
                             s64{static_cast<s32>(a.word(2))} * static_cast<s32>(b.word(2))};
    setWideResult(cpu, rd, products);
}
inline void pmultuw(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    const Reg128 a = cpu.gpr[rs], b = cpu.gpr[rt];
    const s64 products[2] = {
        static_cast<s64>(u64{a.word(0)} * b.word(0)),
        static_cast<s64>(u64{a.word(2)} * b.word(2))};
    setWideResult(cpu, rd, products);
}
inline void pmaddw(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    const Reg128 a = cpu.gpr[rs], b = cpu.gpr[rt];
    const s64 products[2] = {
        static_cast<s64>(multiplyAccumulator(cpu, false)) +
            s64{static_cast<s32>(a.word(0))} * static_cast<s32>(b.word(0)),
        static_cast<s64>(multiplyAccumulator(cpu, true)) +
            s64{static_cast<s32>(a.word(2))} * static_cast<s32>(b.word(2))};
    setWideResult(cpu, rd, products);
}
inline void pmadduw(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    const Reg128 a = cpu.gpr[rs], b = cpu.gpr[rt];
    const s64 products[2] = {
        static_cast<s64>(multiplyAccumulator(cpu, false) + u64{a.word(0)} * b.word(0)),
        static_cast<s64>(multiplyAccumulator(cpu, true) + u64{a.word(2)} * b.word(2))};
    setWideResult(cpu, rd, products);
}
inline void pmsubw(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    const Reg128 a = cpu.gpr[rs], b = cpu.gpr[rt];
    const s64 products[2] = {
        static_cast<s64>(multiplyAccumulator(cpu, false)) -
            s64{static_cast<s32>(a.word(0))} * static_cast<s32>(b.word(0)),
        static_cast<s64>(multiplyAccumulator(cpu, true)) -
            s64{static_cast<s32>(a.word(2))} * static_cast<s32>(b.word(2))};
    setWideResult(cpu, rd, products);
}

inline void pdivw(R5900& cpu, u32 rs, u32 rt) {
    const Reg128 a = cpu.gpr[rs], b = cpu.gpr[rt];
    divideSigned(cpu, static_cast<s32>(a.word(0)), static_cast<s32>(b.word(0)), false);
    divideSigned(cpu, static_cast<s32>(a.word(2)), static_cast<s32>(b.word(2)), true);
}
inline void pdivuw(R5900& cpu, u32 rs, u32 rt) {
    const Reg128 a = cpu.gpr[rs], b = cpu.gpr[rt];
    divideUnsigned(cpu, a.word(0), b.word(0), false);
    divideUnsigned(cpu, a.word(2), b.word(2), true);
}

// Every word of rs is divided by the same halfword of rt, and the four
// quotients and remainders pack into LO and HI as halfwords.
inline void pdivbw(R5900& cpu, u32 rs, u32 rt) {
    const Reg128 a = cpu.gpr[rs];
    const s16 divisor = static_cast<s16>(half(cpu.gpr[rt], 0));
    u32 quotients[4];
    u32 remainders[4];
    for (s32 i = 0; i < 4; ++i) {
        const Quotient result = divideSigned(static_cast<s32>(a.word(i)), divisor);
        quotients[i] = result.quotient;
        remainders[i] = result.remainder;
    }
    cpu.lo = lanes(quotients[0], quotients[1], quotients[2], quotients[3]);
    cpu.hi = lanes(remainders[0], remainders[1], remainders[2], remainders[3]);
}

// The halfword multiplies spread eight 32-bit products over HI and LO, and rd
// takes the same four of them that pmfhl.lw would read back.
inline void spreadHalfProducts(R5900& cpu, u32 rd, const u32 products[8]) {
    cpu.lo = lanes(products[0], products[1], products[4], products[5]);
    cpu.hi = lanes(products[2], products[3], products[6], products[7]);
    cpu.write128(rd, lanes(products[0], products[2], products[4], products[6]));
}

inline void pmulth(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    const Reg128 a = cpu.gpr[rs], b = cpu.gpr[rt];
    u32 products[8];
    for (s32 i = 0; i < 8; ++i) {
        products[i] = static_cast<u32>(s32{static_cast<s16>(half(a, i))} *
                                       static_cast<s16>(half(b, i)));
    }
    spreadHalfProducts(cpu, rd, products);
}
inline void pmaddh(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    const Reg128 a = cpu.gpr[rs], b = cpu.gpr[rt];
    const u32 accumulator[8] = {cpu.lo.word(0), cpu.lo.word(1), cpu.hi.word(0), cpu.hi.word(1),
                                cpu.lo.word(2), cpu.lo.word(3), cpu.hi.word(2), cpu.hi.word(3)};
    u32 products[8];
    for (s32 i = 0; i < 8; ++i) {
        products[i] = accumulator[i] + static_cast<u32>(s32{static_cast<s16>(half(a, i))} *
                                                        static_cast<s16>(half(b, i)));
    }
    spreadHalfProducts(cpu, rd, products);
}
inline void pmsubh(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    const Reg128 a = cpu.gpr[rs], b = cpu.gpr[rt];
    const u32 accumulator[8] = {cpu.lo.word(0), cpu.lo.word(1), cpu.hi.word(0), cpu.hi.word(1),
                                cpu.lo.word(2), cpu.lo.word(3), cpu.hi.word(2), cpu.hi.word(3)};
    u32 products[8];
    for (s32 i = 0; i < 8; ++i) {
        products[i] = accumulator[i] - static_cast<u32>(s32{static_cast<s16>(half(a, i))} *
                                                        static_cast<s16>(half(b, i)));
    }
    spreadHalfProducts(cpu, rd, products);
}

// The subtracting form stores the product in one's complement.
inline void horizontalHalfMultiply(R5900& cpu, u32 rd, u32 rs, u32 rt, bool subtract) {
    const Reg128 a = cpu.gpr[rs], b = cpu.gpr[rt];
    u32 sums[4];
    u32 upper[4];
    for (s32 i = 0; i < 4; ++i) {
        const s32 high = s32{static_cast<s16>(half(a, 2 * i + 1))} *
                         static_cast<s16>(half(b, 2 * i + 1));
        const s32 low32 = s32{static_cast<s16>(half(a, 2 * i))} * static_cast<s16>(half(b, 2 * i));
        sums[i] = static_cast<u32>(subtract ? (high - low32) : (high + low32));
        upper[i] = subtract ? ~static_cast<u32>(high) : static_cast<u32>(high);
    }
    cpu.lo = lanes(sums[0], upper[0], sums[2], upper[2]);
    cpu.hi = lanes(sums[1], upper[1], sums[3], upper[3]);
    cpu.write128(rd, lanes(sums[0], sums[1], sums[2], sums[3]));
}
inline void phmadh(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    horizontalHalfMultiply(cpu, rd, rs, rt, false);
}
inline void phmsbh(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    horizontalHalfMultiply(cpu, rd, rs, rt, true);
}

inline void pmfhl_uw(R5900& cpu, u32 rd) {
    cpu.write128(rd, lanes(cpu.lo.word(1), cpu.hi.word(1), cpu.lo.word(3), cpu.hi.word(3)));
}
inline void pmfhl_slw(R5900& cpu, u32 rd) {
    const s64 first = static_cast<s64>((u64{cpu.hi.word(0)} << 32) | cpu.lo.word(0));
    const s64 second = static_cast<s64>((u64{cpu.hi.word(2)} << 32) | cpu.lo.word(2));
    cpu.write128(rd, Reg128{static_cast<u64>(s64{saturate<s32>(first)}),
                            static_cast<u64>(s64{saturate<s32>(second)})});
}
// The even halfwords of each doubleword of LO and HI, LO first within each.
inline void pmfhl_lh(R5900& cpu, u32 rd) {
    u16 out[8];
    for (s32 i = 0; i < 2; ++i) {
        out[4 * i + 0] = half(cpu.lo, 4 * i + 0);
        out[4 * i + 1] = half(cpu.lo, 4 * i + 2);
        out[4 * i + 2] = half(cpu.hi, 4 * i + 0);
        out[4 * i + 3] = half(cpu.hi, 4 * i + 2);
    }
    cpu.write128(rd, fromHalves(out));
}
// The same grouping pmfhl.lh uses, but each whole word saturates into a
// halfword instead of the even half being taken.
inline void pmfhl_sh(R5900& cpu, u32 rd) {
    const auto narrow = [](u32 word) {
        return static_cast<u16>(saturate<s16>(static_cast<s32>(word)));
    };
    u16 out[8];
    for (s32 i = 0; i < 2; ++i) {
        out[4 * i + 0] = narrow(cpu.lo.word(2 * i + 0));
        out[4 * i + 1] = narrow(cpu.lo.word(2 * i + 1));
        out[4 * i + 2] = narrow(cpu.hi.word(2 * i + 0));
        out[4 * i + 3] = narrow(cpu.hi.word(2 * i + 1));
    }
    cpu.write128(rd, fromHalves(out));
}
inline void pmthl_lw(R5900& cpu, u32 rs) {
    const Reg128 a = cpu.gpr[rs];
    cpu.lo.setWord(0, a.word(0));
    cpu.lo.setWord(2, a.word(2));
    cpu.hi.setWord(0, a.word(1));
    cpu.hi.setWord(2, a.word(3));
}

inline void pextub(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    const Reg128 a = cpu.gpr[rs], b = cpu.gpr[rt];
    u8 out[16];
    for (s32 i = 0; i < 8; ++i) {
        out[2 * i] = byte(b, i + 8);
        out[2 * i + 1] = byte(a, i + 8);
    }
    cpu.write128(rd, fromBytes(out));
}
inline void pextuh(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    const Reg128 a = cpu.gpr[rs], b = cpu.gpr[rt];
    u16 out[8];
    for (s32 i = 0; i < 4; ++i) {
        out[2 * i] = half(b, i + 4);
        out[2 * i + 1] = half(a, i + 4);
    }
    cpu.write128(rd, fromHalves(out));
}
inline void pinth(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    const Reg128 a = cpu.gpr[rs], b = cpu.gpr[rt];
    u16 out[8];
    for (s32 i = 0; i < 4; ++i) {
        out[2 * i] = half(b, i);
        out[2 * i + 1] = half(a, i + 4);
    }
    cpu.write128(rd, fromHalves(out));
}
inline void pinteh(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    const Reg128 a = cpu.gpr[rs], b = cpu.gpr[rt];
    u16 out[8];
    for (s32 i = 0; i < 4; ++i) {
        out[2 * i] = half(b, 2 * i);
        out[2 * i + 1] = half(a, 2 * i);
    }
    cpu.write128(rd, fromHalves(out));
}
inline void prevh(R5900& cpu, u32 rd, u32 rt) {
    const Reg128 b = cpu.gpr[rt];
    u16 out[8];
    for (s32 i = 0; i < 4; ++i) {
        out[i] = half(b, 3 - i);
        out[i + 4] = half(b, 7 - i);
    }
    cpu.write128(rd, fromHalves(out));
}
// Both exchange a pair of halfwords inside each doubleword and leave the other
// two alone: the outer pair for pexeh, the inner pair for pexch.
inline void exchangeHalves(R5900& cpu, u32 rd, u32 rt, s32 first, s32 second) {
    const Reg128 b = cpu.gpr[rt];
    u16 out[8];
    for (s32 i = 0; i < 8; ++i) {
        out[i] = half(b, i);
    }
    std::swap(out[first], out[second]);
    std::swap(out[first + 4], out[second + 4]);
    cpu.write128(rd, fromHalves(out));
}
inline void pexeh(R5900& cpu, u32 rd, u32 rt) {
    exchangeHalves(cpu, rd, rt, 0, 2);
}
inline void pexch(R5900& cpu, u32 rd, u32 rt) {
    exchangeHalves(cpu, rd, rt, 1, 2);
}
inline void pexcw(R5900& cpu, u32 rd, u32 rt) {
    const Reg128 b = cpu.gpr[rt];
    cpu.write128(rd, lanes(b.word(0), b.word(2), b.word(1), b.word(3)));
}

// The 16-bit colour forms move between one packed 1555 pixel per word and one
// unpacked 8888 pixel per word.
inline void pext5(R5900& cpu, u32 rd, u32 rt) {
    const Reg128 b = cpu.gpr[rt];
    u32 out[4];
    for (s32 i = 0; i < 4; ++i) {
        const u32 pixel = b.word(i);
        out[i] = ((pixel & 0x1Fu) << 3) | (((pixel >> 5) & 0x1Fu) << 11) |
                 (((pixel >> 10) & 0x1Fu) << 19) | (((pixel >> 15) & 0x1u) << 31);
    }
    cpu.write128(rd, lanes(out[0], out[1], out[2], out[3]));
}
inline void ppac5(R5900& cpu, u32 rd, u32 rt) {
    const Reg128 b = cpu.gpr[rt];
    u32 out[4];
    for (s32 i = 0; i < 4; ++i) {
        const u32 pixel = b.word(i);
        out[i] = ((pixel >> 3) & 0x1Fu) | (((pixel >> 11) & 0x1Fu) << 5) |
                 (((pixel >> 19) & 0x1Fu) << 10) | (((pixel >> 31) & 0x1u) << 15);
    }
    cpu.write128(rd, lanes(out[0], out[1], out[2], out[3]));
}

// The lower four halfwords subtract and the upper four add.
inline void padsbh(R5900& cpu, u32 rd, u32 rs, u32 rt) {
    const Reg128 a = cpu.gpr[rs], b = cpu.gpr[rt];
    u16 out[8];
    for (s32 i = 0; i < 8; ++i) {
        out[i] = static_cast<u16>(i < 4 ? half(a, i) - half(b, i) : half(a, i) + half(b, i));
    }
    cpu.write128(rd, fromHalves(out));
}

inline void maddu(R5900& cpu, u32 rd, u32 rs, u32 rt, bool pipeline1) {
    setMultiplyResult(cpu, rd,
                      multiplyAccumulator(cpu, pipeline1) +
                          static_cast<u64>(low(cpu.gpr[rs])) * low(cpu.gpr[rt]),
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
// SA holds a byte offset, so only its low four bits survive whichever way it
// is written.
inline void mtsa(R5900& cpu, u32 rs) {
    cpu.sa = low(cpu.gpr[rs]) & 0xFu;
}
inline void mtsab(R5900& cpu, u32 rs, u32 imm) {
    cpu.sa = (low(cpu.gpr[rs]) ^ imm) & 0xFu;
}
inline void mtsah(R5900& cpu, u32 rs, u32 imm) {
    cpu.sa = ((low(cpu.gpr[rs]) ^ imm) & 0x7u) * 2u;
}

// COP0 25 windows the performance registers: low bit counter over control,
// next which counter.
inline u32 readPerformance(const R5900& cpu, u32 selector) {
    return (selector & 1u) ? cpu.performance[(selector >> 1) & 1u] : cpu.performanceControl;
}

inline void writePerformance(R5900& cpu, u32 selector, u32 value) {
    if (selector & 1u) {
        cpu.performance[(selector >> 1) & 1u] = value;
    } else if (selector == 0) {
        cpu.performanceControl = value & cop0::kPerformanceControlMask;
    }
}

inline void mfc0(R5900& cpu, u32 rt, u32 rd, u32 selector) {
    cpu.write32(rt, (rd == cop0::kPerformance) ? readPerformance(cpu, selector)
                                               : cpu.cop0[cop0::resolveAlias(rd)]);
}
// The mask names a page size, so a write that does not is rounded to one: a
// pattern reaching bit 13 fills the field, and one that misses it clears it.
inline u32 roundPageMask(u32 value) {
    constexpr u32 kField = 0x01FFE000u;
    return (value & 0x2000u) != 0 ? kField : 0u;
}

// The entry the index register names, moved between it and the four registers
// that describe one.
inline void tlbr(R5900& cpu) {
    const cop0::TlbEntry& entry = cpu.tlb[cpu.cop0[cop0::kIndex] % cop0::kTlbEntries];
    cpu.cop0[cop0::kEntryHi] = entry.high;
    cpu.cop0[cop0::kEntryLo0] = entry.low0;
    cpu.cop0[cop0::kEntryLo1] = entry.low1;
    cpu.cop0[cop0::kPageMask] = entry.pageMask;
}

inline void tlbwi(R5900& cpu, u32 index) {
    cpu.tlb[index % cop0::kTlbEntries] = {cpu.cop0[cop0::kEntryHi], cpu.cop0[cop0::kEntryLo0],
                                          cpu.cop0[cop0::kEntryLo1],
                                          cpu.cop0[cop0::kPageMask]};
}

inline void mtc0(R5900& cpu, u32 rt, u32 rd, u32 selector) {
    if (rd == cop0::kPerformance) {
        writePerformance(cpu, selector, low(cpu.gpr[rt]));
        return;
    }
    const u32 written = low(cpu.gpr[rt]);
    if (rd == cop0::kPageMask) {
        cpu.cop0[rd] = roundPageMask(written);
        return;
    }
    const u32 mask = cop0::getWriteMask(rd);
    cpu.cop0[rd] = (cpu.cop0[rd] & ~mask) | (written & mask);
    // Writing the register the counter is compared against is what withdraws
    // the interrupt it raised.
    if (rd == cop0::kCompare) {
        cpu.cop0[cop0::kCause] &= ~cop0::kCounterInterrupt;
    }
}
inline void di(R5900& cpu) {
    cpu.cop0[cop0::kStatus] &= ~(1u << 16);
}
inline void ei(R5900& cpu) {
    cpu.cop0[cop0::kStatus] |= (1u << 16);
}

inline bool teqTaken(const R5900& cpu, u32 rs, u32 rt) {
    return cpu.gpr[rs].lo == cpu.gpr[rt].lo;
}

inline bool tneTaken(const R5900& cpu, u32 rs, u32 rt) {
    return cpu.gpr[rs].lo != cpu.gpr[rt].lo;
}

inline bool tgeTaken(const R5900& cpu, u32 rs, u32 rt) {
    return signed64(cpu.gpr[rs]) >= signed64(cpu.gpr[rt]);
}

inline bool tgeuTaken(const R5900& cpu, u32 rs, u32 rt) {
    return cpu.gpr[rs].lo >= cpu.gpr[rt].lo;
}

inline bool tltTaken(const R5900& cpu, u32 rs, u32 rt) {
    return signed64(cpu.gpr[rs]) < signed64(cpu.gpr[rt]);
}

inline bool tltuTaken(const R5900& cpu, u32 rs, u32 rt) {
    return cpu.gpr[rs].lo < cpu.gpr[rt].lo;
}

inline bool teqiTaken(const R5900& cpu, u32 rs, s32 imm) {
    return signed64(cpu.gpr[rs]) == imm;
}

inline bool tneiTaken(const R5900& cpu, u32 rs, s32 imm) {
    return signed64(cpu.gpr[rs]) != imm;
}

inline bool tgeiTaken(const R5900& cpu, u32 rs, s32 imm) {
    return signed64(cpu.gpr[rs]) >= imm;
}

inline bool tgeiuTaken(const R5900& cpu, u32 rs, s32 imm) {
    return cpu.gpr[rs].lo >= static_cast<u64>(static_cast<s64>(imm));
}

inline bool tltiTaken(const R5900& cpu, u32 rs, s32 imm) {
    return signed64(cpu.gpr[rs]) < imm;
}

inline bool tltiuTaken(const R5900& cpu, u32 rs, s32 imm) {
    return cpu.gpr[rs].lo < static_cast<u64>(static_cast<s64>(imm));
}
// EPC keeps the first exception's address, so a nested fault does not lose it.
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
inline void cfc1(R5900& cpu, const Fpu& fpu, u32 rt, u32 fs) {
    cpu.write32(rt, fpu.readControl(fs));
}
inline void ctc1(R5900& cpu, Fpu& fpu, u32 rt, u32 fs) {
    fpu.writeControl(fs, low(cpu.gpr[rt]));
}

inline void qmfc2(R5900& cpu, Vu& vu0, u32 rt, u32 vs, bool interlocked) {
    cpu.write128(rt, interlocked ? vu0.readVf(vs) : vu0.takeVf(vs));
}
inline void qmtc2(R5900& cpu, Vu& vu0, u32 rt, u32 vs) {
    vu0.vf[vs] = cpu.gpr[rt];
}
// The interlocked form waits for the unit, so it sees what the unit has rather
// than what the register was left holding.
inline void cfc2(R5900& cpu, Vu& vu0, u32 rt, u32 rd, bool interlocked) {
    if (rd == Vu::kStatus) {
        cpu.write32(rt, vu0.readSeenStatus());
        return;
    }
    if (rd == Vu::kMac) {
        cpu.write32(rt, vu0.readSeenMac());
        return;
    }
    if (rd != Vu::kVpuStat) {
        cpu.write32(rt, vu0.vi[rd]);
        return;
    }
    cpu.write32(rt, interlocked ? vu0.readStatus() : vu0.takeStatus());
}
// Writing the second unit's start address is what launches it, so that one
// register reaches past the file it lives in.
inline void ctc2(R5900& cpu, EeContext& ee, Memory& memory, u32 rt, u32 rd) {
    ee.vu0.writeControl(rd, low(cpu.gpr[rt]));
    if (rd == Vu::kStatus || rd == Vu::kMac || rd == Vu::kClip) {
        ee.vu0.syncFlags();
    }
    if (rd == Vu::kFbrst) {
        ee.vu0.applyForceBreak(low(cpu.gpr[rt]));
    }
    if (rd == Vu::kCmsar1) {
        memory.mmio().startVu1(ee.vu0.vi[Vu::kCmsar1]);
    }
}

// The translator resolved these, so nothing is left to work out at run time.
inline void lbRam(R5900& cpu, Memory& memory, u32 rt, u32 address) {
    cpu.write32(rt, static_cast<u32>(static_cast<s8>(memory.readRam8(address))));
}
inline void lbuRam(R5900& cpu, Memory& memory, u32 rt, u32 address) {
    cpu.write32(rt, memory.readRam8(address));
}
inline void lhRam(R5900& cpu, Memory& memory, u32 rt, u32 address) {
    cpu.write32(rt, static_cast<u32>(static_cast<s16>(memory.readRam16(address))));
}
inline void lhuRam(R5900& cpu, Memory& memory, u32 rt, u32 address) {
    cpu.write32(rt, memory.readRam16(address));
}
inline void lwRam(R5900& cpu, Memory& memory, u32 rt, u32 address) {
    cpu.write32(rt, memory.readRam32(address));
}
inline void lwuRam(R5900& cpu, Memory& memory, u32 rt, u32 address) {
    cpu.write64(rt, memory.readRam32(address));
}
inline void ldRam(R5900& cpu, Memory& memory, u32 rt, u32 address) {
    cpu.write64(rt, memory.readRam64(address));
}
inline void sbRam(R5900& cpu, Memory& memory, u32 rt, u32 address) {
    memory.writeRam8(address, static_cast<u8>(low(cpu.gpr[rt])));
}
inline void shRam(R5900& cpu, Memory& memory, u32 rt, u32 address) {
    memory.writeRam16(address, static_cast<u16>(low(cpu.gpr[rt])));
}
inline void swRam(R5900& cpu, Memory& memory, u32 rt, u32 address) {
    memory.writeRam32(address, low(cpu.gpr[rt]));
}
inline void sdRam(R5900& cpu, Memory& memory, u32 rt, u32 address) {
    memory.writeRam64(address, cpu.gpr[rt].lo);
}

inline void lqRam(R5900& cpu, Memory& memory, u32 rt, u32 address) {
    const u32 aligned = address & ~15u;
    cpu.write128(rt, Reg128{memory.readRam64(aligned), memory.readRam64(aligned + 8)});
}
inline void sqRam(R5900& cpu, Memory& memory, u32 rt, u32 address) {
    const u32 aligned = address & ~15u;
    memory.writeRam64(aligned, cpu.gpr[rt].lo);
    memory.writeRam64(aligned + 8, cpu.gpr[rt].hi);
}

// A register window, which only a word reaches whole. The block is named by
// the caller, so no search stands between the guest and the device.
template <EeMmioBlock block>
inline void lwMmio(R5900& cpu, EeMmio& mmio, Memory& memory, u32 rt, u32 folded) {
    cpu.write32(rt, readMmio32<block>(mmio, memory, folded));
}
template <EeMmioBlock block>
inline void swMmio(R5900& cpu, EeMmio& mmio, Memory& memory, u32 rt, u32 folded) {
    writeMmio32<block>(mmio, memory, folded, low(cpu.gpr[rt]));
}
template <EeMmioBlock block>
inline void ldMmio(R5900& cpu, EeMmio& mmio, Memory& memory, u32 rt, u32 folded) {
    cpu.write64(rt, readMmio64<block>(mmio, memory, folded));
}
template <EeMmioBlock block>
inline void sdMmio(R5900& cpu, EeMmio& mmio, Memory& memory, u32 rt, u32 folded) {
    writeMmio64<block>(mmio, memory, folded, cpu.gpr[rt].lo);
}

template <typename Base>
inline void lb(R5900& cpu, Memory& memory, u32 rt, Base base, s32 offset) {
    cpu.write32(rt, static_cast<u32>(static_cast<s8>(
                        memory.read8(resolveEa(cpu, base, offset)))));
}
template <typename Base>
inline void lbu(R5900& cpu, Memory& memory, u32 rt, Base base, s32 offset) {
    cpu.write32(rt, memory.read8(resolveEa(cpu, base, offset)));
}
template <typename Base>
inline void lh(R5900& cpu, Memory& memory, u32 rt, Base base, s32 offset) {
    cpu.write32(rt, static_cast<u32>(static_cast<s16>(
                        memory.read16(resolveEa(cpu, base, offset)))));
}
template <typename Base>
inline void lhu(R5900& cpu, Memory& memory, u32 rt, Base base, s32 offset) {
    cpu.write32(rt, memory.read16(resolveEa(cpu, base, offset)));
}
template <typename Base>
inline void lw(R5900& cpu, Memory& memory, u32 rt, Base base, s32 offset) {
    cpu.write32(rt, memory.read32(resolveEa(cpu, base, offset)));
}
template <typename Base>
inline void lwu(R5900& cpu, Memory& memory, u32 rt, Base base, s32 offset) {
    cpu.write64(rt, memory.read32(resolveEa(cpu, base, offset)));
}
template <typename Base>
inline void ld(R5900& cpu, Memory& memory, u32 rt, Base base, s32 offset) {
    cpu.write64(rt, memory.read64(resolveEa(cpu, base, offset)));
}
template <typename Base>
inline void lq(R5900& cpu, Memory& memory, u32 rt, Base base, s32 offset) {
    const u32 aligned = resolveEa(cpu, base, offset) & ~15u;
    cpu.write128(rt, Reg128{memory.read64(aligned), memory.read64(aligned + 8)});
}
template <typename Base>
inline void lwc1(R5900& cpu, Fpu& fpu, Memory& memory, u32 ft, Base base, s32 offset) {
    fpu.fpr[ft] = memory.read32(resolveEa(cpu, base, offset));
}
template <typename Base>
inline void lqc2(R5900& cpu, Vu& vu0, Memory& memory, u32 ft, Base base, s32 offset) {
    const u32 aligned = resolveEa(cpu, base, offset) & ~15u;
    vu0.vf[ft] = Reg128{memory.read64(aligned), memory.read64(aligned + 8)};
}

template <typename Base>
inline void sb(R5900& cpu, Memory& memory, u32 rt, Base base, s32 offset) {
    memory.write8(resolveEa(cpu, base, offset), static_cast<u8>(low(cpu.gpr[rt])));
}
template <typename Base>
inline void sh(R5900& cpu, Memory& memory, u32 rt, Base base, s32 offset) {
    memory.write16(resolveEa(cpu, base, offset), static_cast<u16>(low(cpu.gpr[rt])));
}
template <typename Base>
inline void sw(R5900& cpu, Memory& memory, u32 rt, Base base, s32 offset) {
    memory.write32(resolveEa(cpu, base, offset), low(cpu.gpr[rt]));
}
template <typename Base>
inline void sd(R5900& cpu, Memory& memory, u32 rt, Base base, s32 offset) {
    memory.write64(resolveEa(cpu, base, offset), cpu.gpr[rt].lo);
}
template <typename Base>
inline void sq(R5900& cpu, Memory& memory, u32 rt, Base base, s32 offset) {
    const u32 aligned = resolveEa(cpu, base, offset) & ~15u;
    memory.write64(aligned, cpu.gpr[rt].lo);
    memory.write64(aligned + 8, cpu.gpr[rt].hi);
}
template <typename Base>
inline void swc1(R5900& cpu, const Fpu& fpu, Memory& memory, u32 ft, Base base, s32 offset) {
    memory.write32(resolveEa(cpu, base, offset), fpu.fpr[ft]);
}
template <typename Base>
inline void sqc2(R5900& cpu, Vu& vu0, Memory& memory, u32 ft, Base base, s32 offset) {
    const u32 aligned = resolveEa(cpu, base, offset) & ~15u;
    const Reg128 value = vu0.takeVf(ft);
    memory.write64(aligned, value.lo);
    memory.write64(aligned + 8, value.hi);
}

template <typename Base>
inline void lwl(R5900& cpu, Memory& memory, u32 rt, Base base, s32 offset) {
    const u32 address = resolveEa(cpu, base, offset);
    const u32 shift = 8u * (3u - (address & 3u));
    const u32 word = memory.read32(address & ~3u);
    const u32 keep = (shift == 0) ? 0u : (low(cpu.gpr[rt]) & ((1u << shift) - 1u));
    cpu.write32(rt, (word << shift) | keep);
}
// Only the aligned case loads a whole word, and only then does the result
// sign-extend; a partial merge leaves the upper half of the register alone.
template <typename Base>
inline void lwr(R5900& cpu, Memory& memory, u32 rt, Base base, s32 offset) {
    const u32 address = resolveEa(cpu, base, offset);
    const u32 shift = 8u * (address & 3u);
    const u32 word = memory.read32(address & ~3u);
    if (shift == 0) {
        cpu.write32(rt, word);
        return;
    }
    const u32 merged = (word >> shift) | (low(cpu.gpr[rt]) & ~((1u << (32 - shift)) - 1u));
    cpu.write64(rt, (cpu.gpr[rt].lo & 0xFFFFFFFF00000000ull) | merged);
}
template <typename Base>
inline void ldl(R5900& cpu, Memory& memory, u32 rt, Base base, s32 offset) {
    const u32 address = resolveEa(cpu, base, offset);
    const u32 shift = 8u * (7u - (address & 7u));
    const u64 value = memory.read64(address & ~7u);
    const u64 keep = (shift == 0) ? 0ull : (cpu.gpr[rt].lo & ((1ull << shift) - 1ull));
    cpu.write64(rt, (value << shift) | keep);
}
template <typename Base>
inline void ldr(R5900& cpu, Memory& memory, u32 rt, Base base, s32 offset) {
    const u32 address = resolveEa(cpu, base, offset);
    const u32 shift = 8u * (address & 7u);
    const u64 value = memory.read64(address & ~7u);
    const u64 keep = (shift == 0) ? 0ull : (cpu.gpr[rt].lo & ~((1ull << (64 - shift)) - 1ull));
    cpu.write64(rt, (value >> shift) | keep);
}
template <typename Base>
inline void swl(R5900& cpu, Memory& memory, u32 rt, Base base, s32 offset) {
    const u32 address = resolveEa(cpu, base, offset);
    const u32 bytes = (address & 3u) + 1u;
    const u32 shift = 8u * (4u - bytes);
    const u32 word = memory.read32(address & ~3u);
    const u32 mask = (bytes == 4) ? 0xFFFFFFFFu : ((1u << (8 * bytes)) - 1u);
    memory.write32(address & ~3u, (word & ~mask) | ((low(cpu.gpr[rt]) >> shift) & mask));
}
template <typename Base>
inline void swr(R5900& cpu, Memory& memory, u32 rt, Base base, s32 offset) {
    const u32 address = resolveEa(cpu, base, offset);
    const u32 shift = 8u * (address & 3u);
    const u32 word = memory.read32(address & ~3u);
    const u32 mask = (shift == 0) ? 0xFFFFFFFFu : ~((1u << shift) - 1u);
    memory.write32(address & ~3u, (word & ~mask) | ((low(cpu.gpr[rt]) << shift) & mask));
}
template <typename Base>
inline void sdl(R5900& cpu, Memory& memory, u32 rt, Base base, s32 offset) {
    const u32 address = resolveEa(cpu, base, offset);
    const u32 bytes = (address & 7u) + 1u;
    const u32 shift = 8u * (8u - bytes);
    const u64 value = memory.read64(address & ~7u);
    const u64 mask = (bytes == 8) ? ~0ull : ((1ull << (8 * bytes)) - 1ull);
    memory.write64(address & ~7u, (value & ~mask) | ((cpu.gpr[rt].lo >> shift) & mask));
}
template <typename Base>
inline void sdr(R5900& cpu, Memory& memory, u32 rt, Base base, s32 offset) {
    const u32 address = resolveEa(cpu, base, offset);
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
