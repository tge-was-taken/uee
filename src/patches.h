#pragma once

#include "memory.h"
#include "types.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace uee {

struct Patch {
    u32 address = 0;
    std::vector<u32> words;
};

struct GameConfig {
    std::vector<Patch> patches;
    // IOP modules this runtime answers for rather than running the game's copy.
    std::vector<std::string> hostModules;
    // Loaded before the game runs, in order: an import binds to what is resident.
    std::vector<std::string> preload;
    // Guest addresses to report alongside a loop that is spinning on one.
    std::vector<u32> watch;
    // Resolves small-data addresses a block reached from the dispatcher could
    // never work out for itself.
    std::optional<u32> gp;
    // Hard to detect statically, so it is a manual opt-in instead.
    std::vector<std::pair<u32, u32>> readOnlySegments;
    // How many times the IOP's share of a scanline it may spend. Zero lets it
    // run until it has nothing left to do rather than to a share of real time.
    u32 iopClockScale = 1;
};

// Throws std::runtime_error naming the file and the fault.
GameConfig loadGameConfig(const std::string& path);

void applyPatches(Memory& memory, const std::vector<Patch>& patches);

}  // namespace uee
