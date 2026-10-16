#pragma once

#include "runtime.h"

#include "host.h"

#include <filesystem>
#include <functional>
#include <memory>

// What the platform layer gives the runtime, so the runtime needs no platform
// library of its own. Any of it may be left empty.
struct RuntimeHost {
    // Where a player keeps its disc, card and patches, for a run the command
    // line names none of.
    std::function<std::filesystem::path()> getPlayerDirectory;
    // A disc image in that directory, or one the player picked; empty for none.
    std::function<std::filesystem::path(const std::filesystem::path&)> findDisc;
    // Buttons held on anything besides the keyboard, active high.
    std::function<u16()> readButtons;
    std::shared_ptr<uee::AudioSink> audio;
    // Whether the guest waits for real time; the sound is what holds it back.
    std::function<void(bool)> limitSpeed;
};

std::unique_ptr<core::Runtime> makeUeeRuntime(RuntimeHost host);
