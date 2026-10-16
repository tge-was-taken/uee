#include "sdl_gamepad.h"

#include "host.h"
#include "log.h"

#include <SDL3/SDL.h>

#include <utility>

namespace sdl {

namespace {

uee::LoggerChannel logger("sdl");

constexpr std::pair<SDL_GamepadButton, u16> kButtons[] = {
    {SDL_GAMEPAD_BUTTON_SOUTH, uee::kPadCross},
    {SDL_GAMEPAD_BUTTON_EAST, uee::kPadCircle},
    {SDL_GAMEPAD_BUTTON_WEST, uee::kPadSquare},
    {SDL_GAMEPAD_BUTTON_NORTH, uee::kPadTriangle},
    {SDL_GAMEPAD_BUTTON_BACK, uee::kPadSelect},
    {SDL_GAMEPAD_BUTTON_START, uee::kPadStart},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, uee::kPadL1},
    {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, uee::kPadR1},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK, uee::kPadL3},
    {SDL_GAMEPAD_BUTTON_RIGHT_STICK, uee::kPadR3},
    {SDL_GAMEPAD_BUTTON_DPAD_UP, uee::kPadUp},
    {SDL_GAMEPAD_BUTTON_DPAD_DOWN, uee::kPadDown},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, uee::kPadLeft},
    {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, uee::kPadRight},
};

// The pad is digital, so a stick or trigger counts once it is halfway.
constexpr s16 kHalfway = 16384;

// Reads between looks for a gamepad while none is open, about a second of polls.
constexpr u32 kReadsPerLook = 60;

u16 readButtons(SDL_Gamepad* gamepad) {
    u16 held = 0;
    for (const auto& [button, mask] : kButtons) {
        if (SDL_GetGamepadButton(gamepad, button)) {
            held |= mask;
        }
    }
    const s16 x = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX);
    const s16 y = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY);
    const std::pair<bool, u16> axes[] = {
        {x <= -kHalfway, uee::kPadLeft},
        {x >= kHalfway, uee::kPadRight},
        {y <= -kHalfway, uee::kPadUp},
        {y >= kHalfway, uee::kPadDown},
        {SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) >= kHalfway, uee::kPadL2},
        {SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) >= kHalfway, uee::kPadR2},
    };
    for (const auto& [pushed, mask] : axes) {
        if (pushed) {
            held |= mask;
        }
    }
    return held;
}

}  // namespace

Gamepad::Gamepad() : mInitialized(SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
    if (!mInitialized) {
        logger.warn("no gamepads: {}", SDL_GetError());
    }
}

Gamepad::~Gamepad() {
    SDL_CloseGamepad(mGamepad);
    if (mInitialized) {
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    }
}

u16 Gamepad::getHeldButtons() {
    if (!mInitialized) {
        return 0;
    }
    if (mGamepad != nullptr && !SDL_GamepadConnected(mGamepad)) {
        SDL_CloseGamepad(mGamepad);
        mGamepad = nullptr;
    }
    if (mGamepad == nullptr && mReads++ % kReadsPerLook == 0) {
        int count = 0;
        SDL_JoystickID* gamepads = SDL_GetGamepads(&count);
        if (gamepads != nullptr && count > 0) {
            mGamepad = SDL_OpenGamepad(gamepads[0]);
            if (mGamepad != nullptr) {
                logger.info("gamepad: {}", SDL_GetGamepadName(mGamepad));
            }
        }
        SDL_free(gamepads);
    }
    return mGamepad != nullptr ? readButtons(mGamepad) : 0;
}

}  // namespace sdl
