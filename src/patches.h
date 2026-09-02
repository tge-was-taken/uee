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
struct GameConfig {
    std::vector<Patch> patches;
    // IOP modules this runtime answers for rather than running the game's copy.
    std::vector<std::string> hostModules;
    // Loaded before the game runs, in order: an import binds to what is resident.
    std::vector<std::string> preload;
    // Guest addresses to report alongside a loop that is spinning on one.
    std::vector<u32> watch;
    // Zero leaves the game's own sceMpegGetPicture to run.
    u32 mpegGetPicture = 0;
};

GameConfig loadGameConfig(const std::string& path);

void applyPatches(Memory& memory, const std::vector<Patch>& patches);

}  // namespace uee
