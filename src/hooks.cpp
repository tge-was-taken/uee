#include "hooks.h"

#include "memory.h"

#include <algorithm>
#include <format>
#include <optional>

namespace uee {
namespace {

constexpr u32 kTextLimit = 32;
constexpr usize kTextShortest = 2;

// An argument that reads as terminated printable text is worth showing; one
// that is not a pointer reads as unmapped, and one that is reads as bytes.
std::optional<std::string> textAt(const Memory& memory, u32 address) {
    std::string text;
    for (u32 i = 0; i < kTextLimit; ++i) {
        const std::optional<u8> byte = memory.peek8(address + i);
        if (!byte) {
            return std::nullopt;
        }
        if (*byte == 0) {
            return text.size() >= kTextShortest ? std::optional{text} : std::nullopt;
        }
        if (*byte < ' ' || *byte > '~') {
            return std::nullopt;
        }
        text.push_back(static_cast<char>(*byte));
    }
    return std::nullopt;
}

}  // namespace

void Hooks::watch(const Symbols& symbols, std::string_view names) {
    while (!names.empty()) {
        const usize end = names.find(',');
        const std::string_view name = names.substr(0, end);
        if (const std::optional<u32> address = symbols.find(name)) {
            mEntries.push_back({*address, std::string(name)});
            mLog.info("watching {} at {:08x}", name, *address);
        } else {
            mLog.warn("no symbol named {}", name);
        }
        if (end == std::string_view::npos) {
            break;
        }
        names.remove_prefix(end + 1);
    }
    std::sort(mEntries.begin(), mEntries.end(),
              [](const Entry& left, const Entry& right) { return left.address < right.address; });
}

void Hooks::enter(const Memory& memory, u32 pc, u32 a0, u32 a1, u32 a2, u32 a3) {
    const auto found = std::lower_bound(
        mEntries.begin(), mEntries.end(), pc,
        [](const Entry& entry, u32 value) { return entry.address < value; });
    if (found == mEntries.end() || found->address != pc) {
        return;
    }
    ++found->hits;

    const u32 arguments[] = {a0, a1, a2, a3};
    std::string shown;
    for (const u32 argument : arguments) {
        shown += shown.empty() ? "" : ", ";
        shown += std::format("{:08x}", argument);
        if (const std::optional<std::string> text = textAt(memory, argument)) {
            shown += std::format(" \"{}\"", *text);
        }
    }
    mLog.info("{}({})", found->name, shown);
}

u64 Hooks::getHits(std::string_view name) const {
    for (const Entry& entry : mEntries) {
        if (entry.name == name) {
            return entry.hits;
        }
    }
    return 0;
}

}  // namespace uee
