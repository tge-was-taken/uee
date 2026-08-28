#pragma once

#include "memory.h"
#include "r3000a.h"
#include "types.h"

namespace uee {

inline u32 effectiveAddress(const R3000A& cpu, u32 base, s32 offset) {
    return cpu.gpr[base] + static_cast<u32>(offset);
}

inline void sll(R3000A& cpu, u32 rd, u32 rt, u32 sa) {
    cpu.write(rd, cpu.gpr[rt] << sa);
}
inline void srl(R3000A& cpu, u32 rd, u32 rt, u32 sa) {
    cpu.write(rd, cpu.gpr[rt] >> sa);
}
inline void sra(R3000A& cpu, u32 rd, u32 rt, u32 sa) {
    cpu.write(rd, static_cast<u32>(static_cast<s32>(cpu.gpr[rt]) >> sa));
}
inline void sllv(R3000A& cpu, u32 rd, u32 rt, u32 rs) {
    cpu.write(rd, cpu.gpr[rt] << (cpu.gpr[rs] & 31));
}
inline void srlv(R3000A& cpu, u32 rd, u32 rt, u32 rs) {
    cpu.write(rd, cpu.gpr[rt] >> (cpu.gpr[rs] & 31));
}
inline void srav(R3000A& cpu, u32 rd, u32 rt, u32 rs) {
    cpu.write(rd, static_cast<u32>(static_cast<s32>(cpu.gpr[rt]) >> (cpu.gpr[rs] & 31)));
}

// The trapping forms are not modelled, so add is addu and sub is subu.
inline void addu(R3000A& cpu, u32 rd, u32 rs, u32 rt) {
    cpu.write(rd, cpu.gpr[rs] + cpu.gpr[rt]);
}
inline void subu(R3000A& cpu, u32 rd, u32 rs, u32 rt) {
    cpu.write(rd, cpu.gpr[rs] - cpu.gpr[rt]);
}
inline void and_(R3000A& cpu, u32 rd, u32 rs, u32 rt) {
    cpu.write(rd, cpu.gpr[rs] & cpu.gpr[rt]);
}
inline void or_(R3000A& cpu, u32 rd, u32 rs, u32 rt) {
    cpu.write(rd, cpu.gpr[rs] | cpu.gpr[rt]);
}
inline void xor_(R3000A& cpu, u32 rd, u32 rs, u32 rt) {
    cpu.write(rd, cpu.gpr[rs] ^ cpu.gpr[rt]);
}
inline void nor(R3000A& cpu, u32 rd, u32 rs, u32 rt) {
    cpu.write(rd, ~(cpu.gpr[rs] | cpu.gpr[rt]));
}
inline void slt(R3000A& cpu, u32 rd, u32 rs, u32 rt) {
    cpu.write(rd, static_cast<s32>(cpu.gpr[rs]) < static_cast<s32>(cpu.gpr[rt]));
}
inline void sltu(R3000A& cpu, u32 rd, u32 rs, u32 rt) {
    cpu.write(rd, cpu.gpr[rs] < cpu.gpr[rt]);
}

inline void addiu(R3000A& cpu, u32 rt, u32 rs, s32 imm) {
    cpu.write(rt, cpu.gpr[rs] + static_cast<u32>(imm));
}
inline void andi(R3000A& cpu, u32 rt, u32 rs, u32 imm) {
    cpu.write(rt, cpu.gpr[rs] & imm);
}
inline void ori(R3000A& cpu, u32 rt, u32 rs, u32 imm) {
    cpu.write(rt, cpu.gpr[rs] | imm);
}
inline void xori(R3000A& cpu, u32 rt, u32 rs, u32 imm) {
    cpu.write(rt, cpu.gpr[rs] ^ imm);
}
inline void slti(R3000A& cpu, u32 rt, u32 rs, s32 imm) {
    cpu.write(rt, static_cast<s32>(cpu.gpr[rs]) < imm);
}
inline void sltiu(R3000A& cpu, u32 rt, u32 rs, s32 imm) {
    cpu.write(rt, cpu.gpr[rs] < static_cast<u32>(imm));
}
inline void lui(R3000A& cpu, u32 rt, u32 imm) {
    cpu.write(rt, imm << 16);
}

inline void mult(R3000A& cpu, u32 rs, u32 rt) {
    const s64 product = static_cast<s64>(static_cast<s32>(cpu.gpr[rs])) *
                        static_cast<s64>(static_cast<s32>(cpu.gpr[rt]));
    cpu.lo = static_cast<u32>(product);
    cpu.hi = static_cast<u32>(static_cast<u64>(product) >> 32);
}
inline void multu(R3000A& cpu, u32 rs, u32 rt) {
    const u64 product = static_cast<u64>(cpu.gpr[rs]) * cpu.gpr[rt];
    cpu.lo = static_cast<u32>(product);
    cpu.hi = static_cast<u32>(product >> 32);
}

// MIPS leaves signed division by zero and INT_MIN / -1 undefined; these are
// what the hardware produces.
inline void div(R3000A& cpu, u32 rs, u32 rt) {
    const s32 a = static_cast<s32>(cpu.gpr[rs]);
    const s32 b = static_cast<s32>(cpu.gpr[rt]);
    if (b == 0) {
        cpu.lo = a < 0 ? 1u : 0xFFFFFFFFu;
        cpu.hi = static_cast<u32>(a);
        return;
    }
    if (b == -1 && a == static_cast<s32>(0x80000000)) {
        cpu.lo = 0x80000000u;
        cpu.hi = 0;
        return;
    }
    cpu.lo = static_cast<u32>(a / b);
    cpu.hi = static_cast<u32>(a % b);
}
inline void divu(R3000A& cpu, u32 rs, u32 rt) {
    const u32 a = cpu.gpr[rs];
    const u32 b = cpu.gpr[rt];
    cpu.lo = b == 0 ? 0xFFFFFFFFu : a / b;
    cpu.hi = b == 0 ? a : a % b;
}

inline void mfhi(R3000A& cpu, u32 rd) { cpu.write(rd, cpu.hi); }
inline void mflo(R3000A& cpu, u32 rd) { cpu.write(rd, cpu.lo); }
inline void mthi(R3000A& cpu, u32 rs) { cpu.hi = cpu.gpr[rs]; }
inline void mtlo(R3000A& cpu, u32 rs) { cpu.lo = cpu.gpr[rs]; }

inline void mfc0(R3000A& cpu, u32 rt, u32 rd) { cpu.write(rt, cpu.cop0[rd]); }
inline void mtc0(R3000A& cpu, u32 rt, u32 rd) { cpu.cop0[rd] = cpu.gpr[rt]; }

// Status stacks three (interrupt enable, user mode) pairs in its low six bits.
// The oldest pair stays where it is.
inline void rfe(R3000A& cpu) {
    const u32 status = cpu.cop0[r3000::kStatus];
    cpu.cop0[r3000::kStatus] = (status & ~0x0Fu) | ((status >> 2) & 0x0Fu);
}

constexpr bool beqTaken(const R3000A& cpu, u32 rs, u32 rt) {
    return cpu.gpr[rs] == cpu.gpr[rt];
}
constexpr bool bneTaken(const R3000A& cpu, u32 rs, u32 rt) {
    return cpu.gpr[rs] != cpu.gpr[rt];
}
constexpr bool blezTaken(const R3000A& cpu, u32 rs) {
    return static_cast<s32>(cpu.gpr[rs]) <= 0;
}
constexpr bool bgtzTaken(const R3000A& cpu, u32 rs) {
    return static_cast<s32>(cpu.gpr[rs]) > 0;
}
constexpr bool bltzTaken(const R3000A& cpu, u32 rs) {
    return static_cast<s32>(cpu.gpr[rs]) < 0;
}
constexpr bool bgezTaken(const R3000A& cpu, u32 rs) {
    return static_cast<s32>(cpu.gpr[rs]) >= 0;
}

inline void lb(R3000A& cpu, Memory& memory, u32 rt, u32 base, s32 offset) {
    cpu.delayLoad(rt, static_cast<u32>(static_cast<s8>(
                          memory.read8(effectiveAddress(cpu, base, offset)))));
}
inline void lbu(R3000A& cpu, Memory& memory, u32 rt, u32 base, s32 offset) {
    cpu.delayLoad(rt, memory.read8(effectiveAddress(cpu, base, offset)));
}
inline void lh(R3000A& cpu, Memory& memory, u32 rt, u32 base, s32 offset) {
    cpu.delayLoad(rt, static_cast<u32>(static_cast<s16>(
                          memory.read16(effectiveAddress(cpu, base, offset)))));
}
inline void lhu(R3000A& cpu, Memory& memory, u32 rt, u32 base, s32 offset) {
    cpu.delayLoad(rt, memory.read16(effectiveAddress(cpu, base, offset)));
}
inline void lw(R3000A& cpu, Memory& memory, u32 rt, u32 base, s32 offset) {
    cpu.delayLoad(rt, memory.read32(effectiveAddress(cpu, base, offset)));
}

// Hardware forwards a load still in its delay slot, so an lwr right after an
// lwl on the same register sees the lwl's half.
inline u32 mergeSource(const R3000A& cpu, u32 rt) {
    return cpu.load.reg != 0 && cpu.load.reg == rt ? cpu.load.value : cpu.gpr[rt];
}
inline void lwl(R3000A& cpu, Memory& memory, u32 rt, u32 base, s32 offset) {
    const u32 address = effectiveAddress(cpu, base, offset);
    const u32 shift = 8u * (3u - (address & 3u));
    const u32 word = memory.read32(address & ~3u);
    const u32 keep = (shift == 0) ? 0u : (mergeSource(cpu, rt) & ((1u << shift) - 1u));
    cpu.delayLoad(rt, (word << shift) | keep);
}
inline void lwr(R3000A& cpu, Memory& memory, u32 rt, u32 base, s32 offset) {
    const u32 address = effectiveAddress(cpu, base, offset);
    const u32 shift = 8u * (address & 3u);
    const u32 word = memory.read32(address & ~3u);
    const u32 keep = (shift == 0) ? 0u : (mergeSource(cpu, rt) & ~((1u << (32 - shift)) - 1u));
    cpu.delayLoad(rt, (word >> shift) | keep);
}

inline void sb(R3000A& cpu, Memory& memory, u32 rt, u32 base, s32 offset) {
    memory.write8(effectiveAddress(cpu, base, offset), static_cast<u8>(cpu.gpr[rt]));
}
inline void sh(R3000A& cpu, Memory& memory, u32 rt, u32 base, s32 offset) {
    memory.write16(effectiveAddress(cpu, base, offset), static_cast<u16>(cpu.gpr[rt]));
}
inline void sw(R3000A& cpu, Memory& memory, u32 rt, u32 base, s32 offset) {
    memory.write32(effectiveAddress(cpu, base, offset), cpu.gpr[rt]);
}
inline void swl(R3000A& cpu, Memory& memory, u32 rt, u32 base, s32 offset) {
    const u32 address = effectiveAddress(cpu, base, offset);
    const u32 bytes = (address & 3u) + 1u;
    const u32 shift = 8u * (4u - bytes);
    const u32 word = memory.read32(address & ~3u);
    const u32 mask = (bytes == 4) ? 0xFFFFFFFFu : ((1u << (8 * bytes)) - 1u);
    memory.write32(address & ~3u, (word & ~mask) | ((cpu.gpr[rt] >> shift) & mask));
}
inline void swr(R3000A& cpu, Memory& memory, u32 rt, u32 base, s32 offset) {
    const u32 address = effectiveAddress(cpu, base, offset);
    const u32 shift = 8u * (address & 3u);
    const u32 word = memory.read32(address & ~3u);
    const u32 mask = (shift == 0) ? 0xFFFFFFFFu : ~((1u << shift) - 1u);
    memory.write32(address & ~3u, (word & ~mask) | ((cpu.gpr[rt] << shift) & mask));
}

}  // namespace uee
