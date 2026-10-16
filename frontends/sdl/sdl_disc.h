#pragma once

#include <filesystem>

namespace sdl {

// The directory holding the executable.
std::filesystem::path getBaseDirectory();

// The first disc image in directory, else one the player picks from the
// platform's file dialog; empty when there is none.
std::filesystem::path findDiscImage(const std::filesystem::path& directory);

}  // namespace sdl
