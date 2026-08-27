#include "instruction.h"
#include "opcodes.h"
#include "types.h"

#include <spdlog/spdlog.h>

#include <charconv>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace uee;

s32 failures = 0;

template <typename... Args>
void fail(spdlog::format_string_t<Args...> what, Args&&... args) {
    spdlog::error(what, std::forward<Args>(args)...);
    ++failures;
}

void checkStructure() {
    for (u8 i = 1; i < static_cast<u8>(Table::Count); ++i) {
        const Table id = static_cast<Table>(i);
        const TableInfo& t = lookupTable(id);
        if (t.slots == nullptr) {
            fail("table {}: no slots", id);
            continue;
        }
        const u32 reach = 1u << (t.primaryBits + t.secondaryBits);
        if (reach != t.count) {
            fail("table {}: selector reaches {} slots, table has {}", id, reach, t.count);
        }
        for (u16 slot = 0; slot < t.count; ++slot) {
            const Slot& s = t.slots[slot];
            if (s.sub != Table::None && s.op != Op::Invalid) {
                fail("{}[{:#04x}]: both an op ({}) and a sub-table ({})", id, slot, s.op, s.sub);
            }
            if (s.op == Op::Invalid && s.sub == Table::None && s.args[0] != Field::None) {
                fail("{}[{:#04x}]: arg {} without an op", id, slot, s.args[0]);
            }
        }
    }
}

void checkFieldAccessors() {
    static constexpr u32 kProbes[] = {0x00000000u, 0xFFFFFFFFu, 0x12345678u,
                                      0x4BEC533Cu, 0xAAAAAAAAu, 0x55555555u};
    for (u32 word : kProbes) {
        const Instruction insn{word};
        const struct {
            Field field;
            u32 got;
        } checks[] = {
            {Field::rs, insn.rs()},   {Field::rt, insn.rt()},
            {Field::rd, insn.rd()},   {Field::sa, insn.sa()},
            {Field::imm, insn.imm()}, {Field::target, insn.target()},
            {Field::dest, insn.dest()},
        };
        for (const auto& c : checks) {
            const u32 want = extract(c.field, word);
            if (c.got != want) {
                fail("{:08x}: accessor {} gives {:#x}, the field table says {:#x}",
                     word, c.field, c.got, want);
            }
        }
        if (insn.simm() != static_cast<s32>(static_cast<s16>(insn.imm()))) {
            fail("{:08x}: simm does not sign-extend imm", word);
        }
    }
}

bool isDestMask(std::string_view s) {
    if (s.empty() || s.size() > 4) {
        return false;
    }
    for (char c : s) {
        if (c != 'x' && c != 'y' && c != 'z' && c != 'w') {
            return false;
        }
    }
    return true;
}

// The reference names a few encodings differently: it spells the degenerate
// form of an encoding, and appends the VU destination mask to the mnemonic.
// None of these differences changes what the instruction does.
bool namesMatch(std::string_view ours, std::string_view reference) {
    if (ours == reference) {
        return true;
    }
    const usize dot = reference.rfind('.');
    if (dot != std::string_view::npos && isDestMask(reference.substr(dot + 1)) &&
        reference.substr(0, dot) == ours) {
        return true;
    }
    struct Alias {
        std::string_view reference;
        std::string_view ours;
    };
    static constexpr Alias kAliases[] = {
        {"nop", "sll"},        {"SYNC", "sync.p"},    {"sync", "sync.p"},
        {"qmfc2", "qmfc2.ni"}, {"qmtc2", "qmtc2.ni"},
        {"cfc2", "cfc2.ni"},   {"ctc2", "ctc2.ni"},
    };
    for (const Alias& a : kAliases) {
        if (reference == a.reference && ours == a.ours) {
            return true;
        }
    }
    return false;
}

s32 checkVectors(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) {
        spdlog::critical("cannot read {}", path.string());
        std::exit(2);
    }
    s32 n = 0;
    std::string line;
    while (std::getline(in, line)) {
        const usize tab = line.find('\t');
        if (tab == std::string::npos) {
            continue;
        }
        const std::string_view text(line);
        const std::string_view digits = text.substr(0, tab);
        const std::string_view expected = text.substr(tab + 1);
        u32 word = 0;
        const auto [end, ec] =
            std::from_chars(digits.data(), digits.data() + digits.size(), word, 16);
        if (ec != std::errc{} || end != digits.data() + digits.size()) {
            fail("line {}: '{}' is not a hex word", n + 1, digits);
            continue;
        }
        ++n;
        const Slot& s = decode(Instruction{word});
        if (s.op == Op::Invalid) {
            fail("{:08x}: decodes to nothing, reference says {}", word, expected);
        } else if (!namesMatch(lookupName(s.op), expected)) {
            fail("{:08x}: table says {}, reference says {}", word, s.op, expected);
        }
    }
    return n;
}

}  // namespace

int main(int argc, char** argv) {
    spdlog::set_pattern("%^%l%$: %v");
    const std::filesystem::path path =
        argc > 1 ? argv[1] : std::filesystem::path("tests/vectors/decode.tsv");
    checkStructure();
    checkFieldAccessors();
    const s32 n = checkVectors(path);
    if (failures != 0) {
        spdlog::error("{} failures", failures);
        return 1;
    }
    spdlog::info("{} real words decoded, every name agrees with the reference", n);
    return 0;
}
