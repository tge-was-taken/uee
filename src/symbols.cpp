#include "symbols.h"

#include "log.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <fstream>

namespace uee {
namespace {

LoggerChannel log("symbols");

std::vector<std::pair<u32, std::string>> readMap(const std::string& path) {
    std::vector<std::pair<u32, std::string>> entries;
    std::ifstream file{path};
    if (!file) {
        return entries;
    }
    for (std::string line; std::getline(file, line);) {
        const usize split = line.find(' ');
        if (line.empty() || line[0] == '#' || split == std::string::npos) {
            continue;
        }
        u32 address = 0;
        const char* first = line.data() + (line.starts_with("0x") ? 2 : 0);
        if (std::from_chars(first, line.data() + split, address, 16).ec != std::errc{}) {
            continue;
        }
        std::string name = line.substr(split + 1);
        // The maps are written by a tool on Windows, so the line ends in CRLF.
        while (!name.empty() && (name.back() == '\r' || name.back() == '\n')) {
            name.pop_back();
        }
        entries.emplace_back(address, std::move(name));
    }
    return entries;
}

}  // namespace

void Symbols::setSearchPath(std::string_view paths) {
    mSearchPaths.clear();
    while (!paths.empty()) {
        const usize end = paths.find(':');
        mSearchPaths.emplace_back(paths.substr(0, end));
        if (end == std::string_view::npos) {
            break;
        }
        paths.remove_prefix(end + 1);
    }
}

void Symbols::load(const std::string& module, u32 base, u32 size) {
    std::vector<std::pair<u32, std::string>> entries;
    for (const std::string& search : mSearchPaths) {
        entries = readMap(std::format("{}/{}.map", search, module));
        if (!entries.empty()) {
            break;
        }
    }
    Module loaded{module, base, size, {}};
    loaded.entries.reserve(entries.size());
    for (auto& [address, name] : entries) {
        loaded.entries.push_back({address, std::move(name)});
    }
    std::sort(loaded.entries.begin(), loaded.entries.end(),
              [](const Entry& left, const Entry& right) { return left.offset < right.offset; });
    log.info("{} names for {} at {:08x}..{:08x}", loaded.entries.size(), module, base,
             base + size);
    mModules.push_back(std::move(loaded));
}

std::string Symbols::describe(u32 address) const {
    for (const Module& module : mModules) {
        if (address < module.base || address - module.base >= module.size) {
            continue;
        }
        const u32 offset = address - module.base;
        const auto after =
            std::upper_bound(module.entries.begin(), module.entries.end(), offset,
                             [](u32 value, const Entry& entry) { return value < entry.offset; });
        if (after == module.entries.begin()) {
            return std::format("{}+0x{:x} ({:08x})", module.name, offset, address);
        }
        const Entry& entry = *std::prev(after);
        const u32 into = offset - entry.offset;
        if (into == 0) {
            return std::format("{}!{} ({:08x})", module.name, entry.name, address);
        }
        return std::format("{}!{}+0x{:x} ({:08x})", module.name, entry.name, into, address);
    }
    return std::format("{:08x}", address);
}

std::optional<std::string_view> Symbols::nameAt(u32 address) const {
    for (const Module& module : mModules) {
        if (address < module.base || address - module.base >= module.size) {
            continue;
        }
        const u32 offset = address - module.base;
        const auto found =
            std::lower_bound(module.entries.begin(), module.entries.end(), offset,
                             [](const Entry& entry, u32 value) { return entry.offset < value; });
        if (found != module.entries.end() && found->offset == offset) {
            return std::string_view(found->name);
        }
    }
    return std::nullopt;
}

std::optional<u32> Symbols::find(std::string_view name) const {
    for (const Module& module : mModules) {
        for (const Entry& entry : module.entries) {
            if (entry.name == name) {
                return module.base + entry.offset;
            }
        }
    }
    return std::nullopt;
}

}  // namespace uee
