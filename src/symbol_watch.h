#pragma once

#include "log.h"
#include "symbols.h"
#include "types.h"

#include <string>
#include <string_view>
#include <vector>

namespace uee {

class Memory;

class SymbolWatch {
public:
    void watch(const Symbols& symbols, std::string_view names);

    bool empty() const { return mEntries.empty(); }

    void enter(const Memory& memory, u32 pc, u32 a0, u32 a1, u32 a2, u32 a3);

    u64 getHits(std::string_view name) const;

private:
    struct Entry {
        u32 address = 0;
        std::string name;
        u64 hits = 0;
    };

    LoggerChannel mLog{"hooks"};
    std::vector<Entry> mEntries;
};

}  // namespace uee
