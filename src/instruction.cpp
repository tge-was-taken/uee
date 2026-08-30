#include "instruction.h"

#include "handlers.h"

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

ControlFlow controlFlow(Op op, Instruction insn, u32 pc) {
    switch (op) {
        case Op::beq: case Op::bne: case Op::blez: case Op::bgtz:
        case Op::bltz: case Op::bgez:
        case Op::bc1f: case Op::bc1t:
        case Op::bc0f: case Op::bc0t:
            return {Flow::Branch, branchTarget(insn, pc), 0, 0};

        case Op::beql: case Op::bnel: case Op::blezl: case Op::bgtzl:
        case Op::bltzl: case Op::bgezl:
        case Op::bc1fl: case Op::bc1tl:
        case Op::bc0fl: case Op::bc0tl:
            return {Flow::BranchLikely, branchTarget(insn, pc), 0, 0};

        case Op::bltzal: case Op::bgezal:
            return {Flow::Branch, branchTarget(insn, pc), 0, 31};

        case Op::bltzall: case Op::bgezall:
            return {Flow::BranchLikely, branchTarget(insn, pc), 0, 31};

        case Op::j:
            return {Flow::Jump, jumpTarget(insn, pc), 0, 0};
        case Op::jal:
            return {Flow::Jump, jumpTarget(insn, pc), 0, 31};
        case Op::jr:
            return {Flow::JumpRegister, 0, insn.rs(), 0};
        case Op::jalr:
            return {Flow::JumpRegister, 0, insn.rs(), insn.rd()};

        case Op::syscall: case Op::break_: case Op::eret:
            return {Flow::Trap, 0, 0, 0};

        default:
            return {};
    }
}

namespace reg {

std::string_view nameOf(u32 index) {
    static constexpr std::string_view names[32] = {
        "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2",
        "t3",   "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5",
        "s6",   "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};
    return names[index & 31];
}

}  // namespace reg

namespace {

std::string_view gprName(u32 index) {
    return reg::nameOf(index);
}

bool namesFloatRegisters(std::string_view mnemonic) {
    return mnemonic.find('.') != std::string_view::npos;
}

std::string operand(Field f, Instruction insn, std::string_view mnemonic) {
    const u32 value = insn.operand(f);
    switch (f) {
        case Field::rs:
        case Field::rt:
        case Field::rd:
        case Field::sa:
            if (f == Field::sa && !namesFloatRegisters(mnemonic)) {
                return std::format("{}", value);
            }
            return namesFloatRegisters(mnemonic) ? std::format("f{}", value)
                                                 : std::string(gprName(value));
        default:
            return std::format("{:#x}", value);
    }
}

// The logical ops widen their immediate with zeroes, the arithmetic ones with
// the sign bit, so only the latter read correctly as a negative number.
bool signExtendsImmediate(Op op) {
    switch (op) {
        case Op::addi: case Op::addiu: case Op::daddi: case Op::daddiu:
        case Op::slti: case Op::sltiu:
            return true;
        default:
            return false;
    }
}

std::string immediate(s32 value) {
    return value < 0 ? std::format("-{:#x}", -value) : std::format("{:#x}", value);
}

bool isMemoryForm(const Slot& s) {
    return s.args[0] == Field::rt && s.args[1] == Field::imm && s.args[2] == Field::rs;
}

}  // namespace

std::string disassemble(Instruction insn, u32 pc) {
    const Slot& s = decode(insn);
    if (s.op == Op::Invalid) {
        return std::format("{:08x} <invalid>", insn.word);
    }
    const std::string_view mnemonic = lookupName(s.op);
    std::string text(mnemonic);

    if (isMemoryForm(s)) {
        return text + std::format(" {}, {}({})", gprName(insn.rt()), insn.simm(),
                                  gprName(insn.rs()));
    }

    const ControlFlow cf = controlFlow(s.op, insn, pc);
    for (s32 i = 0; i < 3 && s.args[i] != Field::None; ++i) {
        text += (i == 0) ? " " : ", ";
        if (s.args[i] == Field::target) {
            text += std::format("{:#010x}", jumpTarget(insn, pc));
        } else if (s.args[i] == Field::imm &&
                   (cf.flow == Flow::Branch || cf.flow == Flow::BranchLikely)) {
            text += std::format("{:#010x}", cf.target);
        } else if (s.args[i] == Field::imm && signExtendsImmediate(s.op)) {
            text += immediate(insn.simm());
        } else {
            text += operand(s.args[i], insn, mnemonic);
        }
    }
    return text;
}

std::string disassemble(Instruction insn) {
    return disassemble(insn, 0);
}

std::ostream& operator<<(std::ostream& out, Instruction insn) {
    return out << disassemble(insn);
}

}  // namespace uee
