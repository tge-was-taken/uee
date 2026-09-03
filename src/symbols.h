#pragma once

#include "types.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uee {

class Symbols {
public:
    void setSearchPath(std::string_view paths);
    void load(const std::string& module, u32 base, u32 size);

    // "MODULE!name+0x4 (0053dbcc)", or the address alone when nothing covers it.
    std::string describe(u32 address) const;

    std::optional<u32> find(std::string_view name) const;

    // The name of whatever starts exactly here, for putting a guest function's
    // own name on the code generated from it.
    std::optional<std::string_view> nameAt(u32 address) const;

private:
    struct Entry {
        u32 offset = 0;
        std::string name;
    };

    struct Module {
        std::string name;
        u32 base = 0;
        u32 size = 0;
        std::vector<Entry> entries;
    };

    std::vector<std::string> mSearchPaths;
    std::vector<Module> mModules;
};

}  // namespace uee
