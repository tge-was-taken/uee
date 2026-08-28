#pragma once

#include "memory.h"
#include "types.h"

#include <string>
#include <vector>

namespace uee {

struct Patch {
    u32 address = 0;
    std::vector<u32> words;
};

// Throws std::runtime_error naming the file and the fault.
std::vector<Patch> loadPatches(const std::string& path);

void applyPatches(Memory& memory, const std::vector<Patch>& patches);

}  // namespace uee
