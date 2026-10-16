#include "uee_runtime.h"

#include "sdl_audio.h"
#include "sdl_disc.h"
#include "sdl_gamepad.h"

// The port asks for its runtime here, which is where the SDL pieces are handed in.
std::unique_ptr<core::Runtime> core::makeRuntime() {
    const auto audio = std::make_shared<sdl::Audio>();
    const auto gamepad = std::make_shared<sdl::Gamepad>();
    RuntimeHost host;
    host.getPlayerDirectory = sdl::getBaseDirectory;
    host.findDisc = sdl::findDiscImage;
    host.readButtons = [gamepad] { return gamepad->getHeldButtons(); };
    host.audio = audio;
    host.limitSpeed = [audio](bool limited) { audio->setLimited(limited); };
    return makeUeeRuntime(std::move(host));
}
