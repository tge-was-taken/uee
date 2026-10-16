#include "sdl_audio.h"

#include "log.h"

#include <SDL3/SDL.h>

namespace sdl {

namespace {

uee::LoggerChannel logger("sdl");

// 50 ms: enough to ride out a slow field, little enough to hear as in time.
constexpr int kQueuedBytes =
    static_cast<int>(uee::kSpuSampleRate / 20 * uee::kSpuChannels * sizeof(s16));
// Running unlimited, anything past this would only play late.
constexpr int kMostQueuedBytes = kQueuedBytes * 4;

}  // namespace

Audio::Audio() : mInitialized(SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    if (!mInitialized) {
        logger.warn("no sound: {}", SDL_GetError());
    }
}

Audio::~Audio() {
    SDL_DestroyAudioStream(mStream);
    if (mInitialized) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
}

// The device opens with the first samples, so a run nobody listens to opens none.
bool Audio::open() {
    if (mStream != nullptr) {
        return true;
    }
    if (!mInitialized || mOpenFailed) {
        return false;
    }
    const SDL_AudioSpec spec{SDL_AUDIO_S16, uee::kSpuChannels,
                             static_cast<int>(uee::kSpuSampleRate)};
    mStream =
        SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (mStream == nullptr) {
        logger.warn("no sound, and so no speed limit: {}", SDL_GetError());
        mOpenFailed = true;
        return false;
    }
    SDL_ResumeAudioStreamDevice(mStream);
    logger.info("sound on {} Hz, {} channels", uee::kSpuSampleRate, int{uee::kSpuChannels});
    return true;
}

void Audio::write(std::span<const s16> samples) {
    if (!open()) {
        return;
    }
    if (mLimited.load(std::memory_order_relaxed)) {
        while (SDL_GetAudioStreamQueued(mStream) > kQueuedBytes) {
            SDL_Delay(1);
        }
    } else if (SDL_GetAudioStreamQueued(mStream) > kMostQueuedBytes) {
        SDL_ClearAudioStream(mStream);
    }
    SDL_PutAudioStreamData(mStream, samples.data(), static_cast<int>(samples.size_bytes()));
}

}  // namespace sdl
