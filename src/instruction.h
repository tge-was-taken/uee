#pragma once

#include "opcodes.h"
#include "types.h"

#include <format>
#include <iosfwd>
#include <string>

namespace uee {

namespace reg {
constexpr u32 zero = 0, at = 1, v0 = 2, v1 = 3;
constexpr u32 a0 = 4, a1 = 5, a2 = 6, a3 = 7;
constexpr u32 t0 = 8, t1 = 9, t2 = 10, t3 = 11, t4 = 12, t5 = 13, t6 = 14, t7 = 15;
constexpr u32 s0 = 16, s1 = 17, s2 = 18, s3 = 19, s4 = 20, s5 = 21, s6 = 22, s7 = 23;
constexpr u32 t8 = 24, t9 = 25, k0 = 26, k1 = 27;
constexpr u32 gp = 28, sp = 29, fp = 30, ra = 31;
std::string_view nameOf(u32 index);
}  // namespace reg

// An overlay on one instruction word. Which fields carry meaning is the op's
// business, so every accessor is unconditional.
struct Instruction {
    u32 word = 0;

    constexpr u32 rs() const { return (word >> 21) & 0x1Fu; }
    constexpr u32 rt() const { return (word >> 16) & 0x1Fu; }
    constexpr u32 rd() const { return (word >> 11) & 0x1Fu; }
    constexpr u32 sa() const { return (word >> 6) & 0x1Fu; }
    constexpr u32 imm() const { return word & 0xFFFFu; }
    constexpr s32 simm() const { return static_cast<s16>(word & 0xFFFFu); }
    constexpr u32 target() const { return word & 0x03FFFFFFu; }

    // the coprocessors name the same three fields differently
    constexpr u32 fs() const { return rd(); }
    constexpr u32 ft() const { return rt(); }
    constexpr u32 fd() const { return sa(); }
    constexpr u32 vs() const { return rd(); }
    constexpr u32 vt() const { return rt(); }
    constexpr u32 vd() const { return sa(); }
    constexpr u32 vis() const { return rd(); }
    constexpr u32 vit() const { return rt(); }
    constexpr u32 vid() const { return sa(); }

    constexpr u32 dest() const { return (word >> 21) & 0x0Fu; }
    constexpr s32 imm5() const { return static_cast<s32>(sa() << 27) >> 27; }
    constexpr u32 imm15() const { return (word >> 6) & 0x7FFFu; }
    constexpr u32 fsf() const { return (word >> 21) & 0x3u; }
    constexpr u32 ftf() const { return (word >> 23) & 0x3u; }

    u32 operand(Field f) const { return extract(f, word); }
};

static_assert(sizeof(Instruction) == sizeof(u32));

// Always lands somewhere: an encoding no source names gets a slot whose op is
// Op::Invalid.
const Slot& decode(Instruction insn);

inline Op opOf(Instruction insn) {
    return decode(insn).op;
}

constexpr u32 branchTarget(Instruction insn, u32 pc) {
    return pc + 4 + (static_cast<u32>(insn.simm()) << 2);
}

constexpr u32 jumpTarget(Instruction insn, u32 pc) {
    return ((pc + 4) & 0xF0000000u) | (insn.target() << 2);
}

enum class Flow : u8 {
    Next,          // falls through to pc + 4
    Branch,        // conditional; the delay slot runs either way
    BranchLikely,  // conditional; the delay slot runs only when taken
    Jump,          // unconditional, target known from the encoding
    JumpRegister,  // unconditional, target read from a register
    Trap,          // syscall, break and eret resolve the pc themselves
};

struct ControlFlow {
    Flow flow = Flow::Next;
    u32 target = 0;  // Branch, BranchLikely and Jump
    u32 source = 0;  // JumpRegister: the register holding the target
    // Zero means no link, which is also what linking to r0 would do.
    u32 link = 0;
};

ControlFlow controlFlow(Op op, Instruction insn, u32 pc);

std::string disassemble(Instruction insn);
std::string disassemble(Instruction insn, u32 pc);
std::ostream& operator<<(std::ostream& out, Instruction insn);

}  // namespace uee

template <>
struct std::formatter<uee::Instruction> : std::formatter<std::string> {
    auto format(uee::Instruction insn, std::format_context& ctx) const {
        return std::formatter<std::string>::format(uee::disassemble(insn), ctx);
    }
};
