#pragma once

#include "types.h"

struct SDL_Gamepad;

namespace sdl {

// The first gamepad plugged in, read as the pad's held buttons.
class Gamepad {
public:
    Gamepad();
    ~Gamepad();

    Gamepad(const Gamepad&) = delete;
    Gamepad& operator=(const Gamepad&) = delete;

    // Active high, in libpad's bit order.
    u16 getHeldButtons();

private:
    bool mInitialized = false;
    SDL_Gamepad* mGamepad = nullptr;
    u32 mReads = 0;
};

}  // namespace sdl
