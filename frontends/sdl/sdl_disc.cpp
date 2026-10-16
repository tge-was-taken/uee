#include "sdl_disc.h"

#include "log.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <mutex>
#include <string>
#include <vector>

namespace sdl {

namespace {

uee::LoggerChannel logger("sdl");

// SDL hands back UTF-8, which a narrow path on Windows would read as the ANSI
// code page.
std::filesystem::path getPathOf(const char* utf8) {
    return std::filesystem::path(reinterpret_cast<const char8_t*>(utf8));
}

bool isDiscImage(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".iso";
}

// SDL may answer the dialog on a thread of its own.
struct Choice {
    std::mutex mutex;
    std::filesystem::path path;
    bool done = false;
};

std::filesystem::path pickDiscImage(const std::filesystem::path& directory) {
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        logger.error("cannot ask for a disc: {}", SDL_GetError());
        return {};
    }
    Choice choice;
    const auto answer = [](void* data, const char* const* files, int) {
        Choice& choice = *static_cast<Choice*>(data);
        std::lock_guard lock(choice.mutex);
        if (files != nullptr && files[0] != nullptr) {
            choice.path = getPathOf(files[0]);
        }
        choice.done = true;
    };
    const SDL_DialogFileFilter filters[] = {{"PS2 disc image", "iso"}};
    const std::u8string start = directory.u8string();
    SDL_ShowOpenFileDialog(answer, &choice, nullptr, filters, 1,
                           reinterpret_cast<const char*>(start.c_str()), false);
    for (;;) {
        {
            std::lock_guard lock(choice.mutex);
            if (choice.done) {
                break;
            }
        }
        SDL_PumpEvents();
        SDL_Delay(10);
    }
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return choice.path;
}

}  // namespace

std::filesystem::path getBaseDirectory() {
    const char* base = SDL_GetBasePath();
    return base != nullptr ? getPathOf(base) : std::filesystem::current_path();
}

std::filesystem::path findDiscImage(const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> images;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.is_regular_file() && isDiscImage(entry.path())) {
            images.push_back(entry.path());
        }
    }
    if (images.empty()) {
        return pickDiscImage(directory);
    }
    std::sort(images.begin(), images.end());
    return images.front();
}

}  // namespace sdl
