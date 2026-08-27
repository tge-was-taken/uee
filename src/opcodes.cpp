#include "opcodes.h"

#include <ostream>

namespace uee {
namespace {

#define R5900_TABLE_BEGIN(name, slots, primaryShift, primaryBits, secondaryShift, secondaryBits) \
    constexpr Slot k##name[] = {
#define R5900_TABLE_END(name) };
#define R5900_SUB(index, subTable) {Op::Invalid, Table::subTable, {}},
#define R5900_BAD(index) {Op::Invalid, Table::None, {}},
#define R5900_OP0(index, op) {Op::op, Table::None, {}},
#define R5900_OP1(index, op, a) {Op::op, Table::None, {Field::a}},
#define R5900_OP2(index, op, a, b) {Op::op, Table::None, {Field::a, Field::b}},
#define R5900_OP3(index, op, a, b, c) \
    {Op::op, Table::None, {Field::a, Field::b, Field::c}},
#include "opcode_table.inc"

constexpr TableInfo kTables[] = {
    {nullptr, 0, 0, 0, 0, 0},
#define R5900_TABLE_BEGIN(name, slots, primaryShift, primaryBits, secondaryShift, secondaryBits) \
    {k##name, slots, primaryShift, primaryBits, secondaryShift, secondaryBits},
#include "opcode_table.inc"
};

constexpr std::string_view kOpNames[] = {
    "invalid",
#define R5900_OPCODE(ident, text) text,
#include "opcode_table.inc"
};

constexpr std::string_view kFieldNames[] = {
    "none",
#define R5900_FIELD(name, shift, bits) #name,
#include "opcode_table.inc"
};

constexpr std::string_view kTableNames[] = {
    "none",
#define R5900_TABLE_BEGIN(name, slots, primaryShift, primaryBits, secondaryShift, secondaryBits) #name,
#include "opcode_table.inc"
    "count",
};

struct FieldBits {
    u8 shift;
    u8 bits;
};

constexpr FieldBits kFields[] = {
    {0, 0},
#define R5900_FIELD(name, shift, bits) {shift, bits},
#include "opcode_table.inc"
};

}  // namespace

const TableInfo& lookupTable(Table id) {
    return kTables[static_cast<u8>(id)];
}

std::string_view lookupName(Op op) {
    return kOpNames[static_cast<u16>(op)];
}

std::string_view lookupName(Field f) {
    return kFieldNames[static_cast<u8>(f)];
}

std::string_view lookupName(Table id) {
    return kTableNames[static_cast<u8>(id)];
}

u32 extract(Field f, u32 word) {
    const FieldBits& spec = kFields[static_cast<u8>(f)];
    return (word >> spec.shift) & ((1u << spec.bits) - 1u);
}

std::ostream& operator<<(std::ostream& out, Op op) {
    return out << lookupName(op);
}

std::ostream& operator<<(std::ostream& out, Field f) {
    return out << lookupName(f);
}

std::ostream& operator<<(std::ostream& out, Table id) {
    return out << lookupName(id);
}

}  // namespace uee
