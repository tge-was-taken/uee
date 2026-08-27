#pragma once

#include "types.h"

#include <concepts>
#include <format>
#include <iosfwd>
#include <string_view>

namespace uee {

enum class Op : u16 {
    Invalid = 0,
#define R5900_OPCODE(ident, text) ident,
#include "opcode_table.inc"
};

enum class Field : u8 {
    None = 0,
#define R5900_FIELD(name, shift, bits) name,
#include "opcode_table.inc"
};

enum class Table : u8 {
    None = 0,
#define R5900_TABLE_BEGIN(name, slots, primaryShift, primaryBits, secondaryShift, secondaryBits) name,
#include "opcode_table.inc"
    Count,
};

struct Slot {
    Op op;
    Table sub;
    Field args[3];
};

struct TableInfo {
    const Slot* slots;
    u16 count;
    u8 primaryShift;
    u8 primaryBits;
    u8 secondaryShift;
    u8 secondaryBits;
};

const TableInfo& lookupTable(Table id);
std::string_view lookupName(Op op);
std::string_view lookupName(Field f);
std::string_view lookupName(Table id);
u32 extract(Field f, u32 word);

std::ostream& operator<<(std::ostream& out, Op op);
std::ostream& operator<<(std::ostream& out, Field f);
std::ostream& operator<<(std::ostream& out, Table id);

inline u32 select(const TableInfo& t, u32 word) {
    const u32 primary = (word >> t.primaryShift) & ((1u << t.primaryBits) - 1u);
    const u32 secondary = (word >> t.secondaryShift) & ((1u << t.secondaryBits) - 1u);
    return (primary << t.secondaryBits) | secondary;
}

template <typename T>
concept Named = std::same_as<T, Op> || std::same_as<T, Field> || std::same_as<T, Table>;

}  // namespace uee

// std::format does not consult operator<<, so the enums need a formatter too.
template <uee::Named T>
struct std::formatter<T> : std::formatter<std::string_view> {
    auto format(T value, std::format_context& ctx) const {
        return std::formatter<std::string_view>::format(uee::lookupName(value), ctx);
    }
};
