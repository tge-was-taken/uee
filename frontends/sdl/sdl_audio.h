#pragma once

#include "host.h"

#include <atomic>
#include <span>

struct SDL_AudioStream;

namespace sdl {

// Plays what the sound processor mixes on the default device. While limited, a
// full queue holds the writer back, which runs the guest at the speed its sound
// plays at.
class Audio final : public uee::AudioSink {
public:
    Audio();
    ~Audio() override;

    Audio(const Audio&) = delete;
    Audio& operator=(const Audio&) = delete;

    void setLimited(bool limited) { mLimited.store(limited, std::memory_order_relaxed); }
    void write(std::span<const s16> samples) override;

private:
    bool open();

    bool mInitialized = false;
    std::atomic<bool> mLimited{false};
    SDL_AudioStream* mStream = nullptr;
    bool mOpenFailed = false;
};

}  // namespace sdl
