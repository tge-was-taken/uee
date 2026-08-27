#include "instruction.h"

#include <ostream>

namespace uee {
namespace {

// root -> COP2 -> COP2.CO -> COP2.V is the longest path in the tables
constexpr s32 kMaxDepth = 4;

constexpr Slot kUndecodable = {Op::Invalid, Table::None, {}};

}  // namespace

const Slot& decode(Instruction insn) {
    Table id = Table::Root;
    for (s32 depth = 0; depth < kMaxDepth; ++depth) {
        const TableInfo& t = lookupTable(id);
        // select cannot exceed the slot count: a table holds one slot per value
        // its selector bits can take
        const Slot& s = t.slots[select(t, insn.word)];
        if (s.sub == Table::None) {
            return s;
        }
        id = s.sub;
    }
    return kUndecodable;
}

std::string disassemble(Instruction insn) {
    const Slot& s = decode(insn);
    if (s.op == Op::Invalid) {
        return std::format("{:08x} <invalid>", insn.word);
    }
    std::string text(lookupName(s.op));
    for (s32 i = 0; i < 3 && s.args[i] != Field::None; ++i) {
        text += (i == 0) ? " " : ", ";
        text += std::format("{}={:#x}", s.args[i], insn.operand(s.args[i]));
    }
    return text;
}

std::ostream& operator<<(std::ostream& out, Instruction insn) {
    return out << disassemble(insn);
}

}  // namespace uee
