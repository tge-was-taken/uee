#pragma once

#include "opcodes.h"
#include "types.h"

#include <format>
#include <iosfwd>
#include <string>

namespace uee {

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

std::string disassemble(Instruction insn);
std::ostream& operator<<(std::ostream& out, Instruction insn);

}  // namespace uee

template <>
struct std::formatter<uee::Instruction> : std::formatter<std::string> {
    auto format(uee::Instruction insn, std::format_context& ctx) const {
        return std::formatter<std::string>::format(uee::disassemble(insn), ctx);
    }
};
