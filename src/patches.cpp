#include "patches.h"

#include <toml++/toml.hpp>

#include <cctype>
#include <format>
#include <optional>
#include <string>
#include <stdexcept>

namespace uee {
namespace {

// One is named by file and the other by module.
std::string moduleNameOf(const std::string& file) {
    std::string bare = file.substr(file.find_last_of(":\\/") + 1);
    bare = bare.substr(0, bare.find('.'));
    for (char& letter : bare) {
        letter = static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
    }
    return bare;
}

u32 wordAt(const toml::node& node, const std::string& path, usize index) {
    const std::optional<s64> value = node.value<s64>();
    if (!value || *value < 0 || *value > 0xFFFFFFFF) {
        throw std::runtime_error(
            std::format("{}: patch word {} is not a 32 bit number", path, index));
    }
    return static_cast<u32>(*value);
}

}  // namespace

GameConfig loadGameConfig(const std::string& path) {
    toml::table file;
    try {
        file = toml::parse_file(path);
    } catch (const toml::parse_error& error) {
        throw std::runtime_error(std::format("{}: {}", path, error.description()));
    }

    GameConfig config;
    if (const toml::array* hosted = file["iop"]["host"].as_array()) {
        for (const toml::node& name : *hosted) {
            const std::optional<std::string> module = name.value<std::string>();
            if (!module) {
                throw std::runtime_error(
                    std::format("{}: iop.host names modules, as strings", path));
            }
            config.hostModules.push_back(*module);
        }
    }

    if (const toml::array* first = file["iop"]["preload"].as_array()) {
        for (const toml::node& name : *first) {
            const std::optional<std::string> module = name.value<std::string>();
            if (!module) {
                throw std::runtime_error(
                    std::format("{}: iop.preload names module files, as strings", path));
            }
            config.preload.push_back(*module);
        }
    }

    // The load is what binds imports, and host skips it.
    for (const std::string& module : config.preload) {
        const std::string bare = moduleNameOf(module);
        for (const std::string& hosted : config.hostModules) {
            if (hosted == bare) {
                throw std::runtime_error(std::format(
                    "{}: {} is preloaded and answered for here, which cannot both be true",
                    path, bare));
            }
        }
    }

    const toml::array* entries = file["patch"].as_array();
    if (entries == nullptr) {
        return config;
    }

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
        config.patches.push_back(std::move(patch));
    }
    return config;
}

void applyPatches(Memory& memory, const std::vector<Patch>& patches) {
    for (const Patch& patch : patches) {
        for (usize i = 0; i < patch.words.size(); ++i) {
            memory.write32(patch.address + static_cast<u32>(i) * 4, patch.words[i]);
        }
    }
}

}  // namespace uee
