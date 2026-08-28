#include "patches.h"

#include <toml++/toml.hpp>

#include <format>
#include <stdexcept>

namespace uee {
namespace {

u32 wordAt(const toml::node& node, const std::string& path, usize index) {
    const std::optional<s64> value = node.value<s64>();
    if (!value || *value < 0 || *value > 0xFFFFFFFF) {
        throw std::runtime_error(
            std::format("{}: patch word {} is not a 32 bit number", path, index));
    }
    return static_cast<u32>(*value);
}

}  // namespace

std::vector<Patch> loadPatches(const std::string& path) {
    toml::table file;
    try {
        file = toml::parse_file(path);
    } catch (const toml::parse_error& error) {
        throw std::runtime_error(std::format("{}: {}", path, error.description()));
    }

    const toml::array* entries = file["patch"].as_array();
    if (entries == nullptr) {
        throw std::runtime_error(std::format("{}: wants an array of [[patch]] tables", path));
    }

    std::vector<Patch> patches;
    for (const toml::node& entry : *entries) {
        const toml::table* table = entry.as_table();
        if (table == nullptr) {
            throw std::runtime_error(std::format("{}: every patch has to be a table", path));
        }
        const std::optional<s64> address = (*table)["address"].value<s64>();
        const toml::array* words = (*table)["words"].as_array();
        if (!address || words == nullptr) {
            throw std::runtime_error(
                std::format("{}: every patch wants an address and a words array", path));
        }

        Patch patch;
        patch.address = static_cast<u32>(*address);
        for (usize i = 0; i < words->size(); ++i) {
            patch.words.push_back(wordAt(*words->get(i), path, i));
        }
        patches.push_back(std::move(patch));
    }
    return patches;
}

void applyPatches(Memory& memory, const std::vector<Patch>& patches) {
    for (const Patch& patch : patches) {
        for (usize i = 0; i < patch.words.size(); ++i) {
            memory.write32(patch.address + static_cast<u32>(i) * 4, patch.words[i]);
        }
    }
}

}  // namespace uee
