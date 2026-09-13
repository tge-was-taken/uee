#include "card_file.h"
#include "disc.h"
#include "elf.h"
#include "host.h"
#include "interpreter.h"
#include "kernel.h"
#include "library.h"
#include "log.h"
#include "memory.h"
#include "gs_capture.h"
#include "movie.h"
#include "patches.h"
#include "run.h"
#include "stop.h"
#include "vu_capture.h"
#include "vu_programs.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <functional>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#ifdef UEE_TRANSLATED_P3FES
namespace uee {
bool runFunction(Machine& machine, u32 pc);
}
constexpr uee::FunctionRunner kTranslated = uee::runFunction;
#else
constexpr uee::FunctionRunner kTranslated = nullptr;
#endif

#ifdef UEE_VU_PROGRAMS
namespace uee {
extern const std::span<const TranslatedVuProgram> translatedVuPrograms;
extern const std::span<const TranslatedVuProgram> translatedFastVuPrograms;
}
std::span<const uee::TranslatedVuProgram> getTranslatedVuPrograms(bool fast) {
    return fast ? uee::translatedFastVuPrograms : uee::translatedVuPrograms;
}
#else
std::span<const uee::TranslatedVuProgram> getTranslatedVuPrograms(bool) {
    return {};
}
#endif

namespace {

// Both the renderer and a capture file want the same stream, and taking it
// twice would run the guest twice.
class SplitGs final : public uee::GsHandler {
public:
    SplitGs(uee::GsHandler& first, uee::GsHandler& second) : mFirst(first), mSecond(second) {}

    void begin(std::span<const u8> registers) override {
        mFirst.begin(registers);
        mSecond.begin(registers);
    }
    void transfer(u32 path, std::span<const u8> data) override {
        mFirst.transfer(path, data);
        mSecond.transfer(path, data);
    }
    void vsync(u32 field, std::span<const u8> registers) override {
        mFirst.vsync(field, registers);
        mSecond.vsync(field, registers);
    }

private:
    uee::GsHandler& mFirst;
    uee::GsHandler& mSecond;
};

// The movies play on the graphics unit, not here, so the picture goes nowhere.
class DroppedVideo final : public uee::VideoSink {
public:
    void write(std::span<const u8>) override {}
};

uee::LoggerChannel logger("gs");

constexpr int kMaxWidth = 1024;
constexpr int kMaxHeight = 1024;

// What the library is told about the window it draws into. Mirrors its own
// struct rather than including its header, which is C and platform specific.
struct BorrowedWindow {
    int type = 0;
    void* display = nullptr;
    void* handle = nullptr;
    u32 width = 0;
    u32 height = 0;
    float scale = 1.0f;
};

// PCSX2's graphics synthesiser, loaded rather than linked. It is PCSX2's code
// under PCSX2's licence, and this reaches it only through its C interface.
class GsLibrary {
public:
    bool load(const char* path) {
        std::string error;
        mLibrary = openLibrary(path, error);
        if (mLibrary == nullptr) {
            logger.fatal("cannot load {}: {}", path, error);
            return false;
        }
        open = findSymbol<int (*)(u8*, int, const BorrowedWindow*, const char*, int)>(
            "pcsx2hook_gs_open");
        closeRenderer = findSymbol<void (*)()>("pcsx2hook_gs_close");
        transfer = findSymbol<void (*)(int, const u8*, u32)>("pcsx2hook_gs_transfer");
        endField = findSymbol<void (*)(int)>("pcsx2hook_gs_end_field");
        readDisplay = findSymbol<int (*)(u32*, int, int*, int*)>("pcsx2hook_gs_read_display");
        resize = findSymbol<void (*)(u32, u32, float)>("pcsx2hook_gs_resize");
        writeDisplay = findSymbol<int (*)(const char*)>("pcsx2hook_gs_write_display");
        return open != nullptr && closeRenderer != nullptr && transfer != nullptr &&
               endField != nullptr && readDisplay != nullptr && writeDisplay != nullptr &&
               resize != nullptr;
    }

    int (*open)(u8* registers, int renderer, const BorrowedWindow* window,
                const char* resources, int threads) = nullptr;
    void (*closeRenderer)() = nullptr;
    void (*transfer)(int path, const u8* data, u32 bytes) = nullptr;
    void (*endField)(int field) = nullptr;
    int (*readDisplay)(u32* pixels, int capacity, int* width, int* height) = nullptr;
    void (*resize)(u32 width, u32 height, float scale) = nullptr;
    int (*writeDisplay)(const char* path) = nullptr;

private:
    template <typename Function>
    Function findSymbol(const char* name) {
        void* found = findLibrarySymbol(mLibrary, name);
        if (found == nullptr) {
            logger.fatal("the graphics library does not answer {}", name);
        }
        return reinterpret_cast<Function>(found);
    }

    void* mLibrary = nullptr;
};

std::vector<u8> readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        logger.fatal("cannot open {}", path.string());
        std::exit(2);
    }
    return std::vector<u8>{std::istreambuf_iterator<char>(file),
                           std::istreambuf_iterator<char>()};
}

// The window the picture goes to, and the keyboard the guest reads as a pad.
// Both are only touched between fields, which is where this is called.
class Window : public uee::PadSource, public uee::StopRequest {
public:
    bool open(const std::string& title) {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            logger.fatal("cannot start sdl: {}", SDL_GetError());
            return false;
        }
        mWindow = SDL_CreateWindow(title.c_str(), 640, 448, SDL_WINDOW_RESIZABLE);
        if (mWindow == nullptr) {
            logger.fatal("cannot open a window: {}", SDL_GetError());
            return false;
        }
        mTitle = title;
        mRateStart = std::chrono::steady_clock::now();
        // No renderer yet: one binds a graphics context to the window, and a
        // renderer that draws into the window itself must be the only one.
        return true;
    }

    ~Window() override {
        SDL_DestroyRenderer(mRenderer);
        SDL_DestroyWindow(mWindow);
        SDL_Quit();
    }

    bool isStopRequested() override { return mClosing; }

    void show(const u32* pixels, int width, int height) {
        if (mRenderer == nullptr) {
            mRenderer = SDL_CreateRenderer(mWindow, nullptr);
            if (mRenderer == nullptr) {
                logger.fatal("cannot draw into the window: {}", SDL_GetError());
                mClosing = true;
                return;
            }
        }
        if (width != mWidth || height != mHeight) {
            SDL_DestroyTexture(mTexture);
            mTexture = SDL_CreateTexture(mRenderer, SDL_PIXELFORMAT_ABGR8888,
                                         SDL_TEXTUREACCESS_STREAMING, width, height);
            mWidth = width;
            mHeight = height;
        }
        SDL_UpdateTexture(mTexture, nullptr, pixels, width * 4);
        SDL_RenderClear(mRenderer);
        SDL_RenderTexture(mRenderer, mTexture, nullptr, nullptr);
        SDL_RenderPresent(mRenderer);
    }

    // What the renderer needs to draw into this window, per platform. Anything
    // not named here shows nothing and the picture is read back instead.
    BorrowedWindow getBorrowedWindow() const {
        BorrowedWindow window;
        window.scale = SDL_GetWindowDisplayScale(mWindow);
        int width = 0;
        int height = 0;
        SDL_GetWindowSizeInPixels(mWindow, &width, &height);
        window.width = static_cast<u32>(width);
        window.height = static_cast<u32>(height);

        SDL_PropertiesID properties = SDL_GetWindowProperties(mWindow);
        const std::string driver = SDL_GetCurrentVideoDriver();
        if (driver == "x11") {
            window.type = 2;
            window.display =
                SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
            window.handle = reinterpret_cast<void*>(
                SDL_GetNumberProperty(properties, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
        } else if (driver == "wayland") {
            window.type = 3;
            window.display = SDL_GetPointerProperty(
                properties, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
            window.handle = SDL_GetPointerProperty(
                properties, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
        } else if (driver == "windows") {
            window.type = 1;
            window.handle =
                SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
        } else if (driver == "cocoa") {
            // The renderer wants the view, and nothing here has read one out of
            // the window: untested, and macOS draws with Metal or not at all.
            window.type = 0;
        }
        return window;
    }

    void pollEvents() {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                mClosing = true;
            }
            if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                mResized = true;
            }
        }
    }

    void countField() {
        ++mRateFields;
        const auto now = std::chrono::steady_clock::now();
        const std::chrono::duration<double> elapsed = now - mRateStart;
        if (elapsed < std::chrono::seconds(1)) {
            return;
        }
        const double rate = mRateFields / elapsed.count();
        SDL_SetWindowTitle(mWindow, std::format("{} ({:.1f} fields/s)", mTitle, rate).c_str());
        logger.trace("{:.1f} fields/s", rate);
        mRateStart = now;
        mRateFields = 0;
    }

    bool takeResize() {
        const bool resized = mResized;
        mResized = false;
        return resized;
    }

    // Pressed and released every twenty polls, so a run can walk menus unattended.
    void pressRepeatedly(u16 buttons) { mRepeated = buttons; }

    // Active low, as the pad reports it.
    u16 buttons() override {
        const bool* keys = SDL_GetKeyboardState(nullptr);
        u16 held = mPolls++ / 20 % 2 == 0 ? mRepeated : 0;
        held |= keys[SDL_SCANCODE_LEFT] ? 0x8000 : 0;
        held |= keys[SDL_SCANCODE_DOWN] ? 0x4000 : 0;
        held |= keys[SDL_SCANCODE_RIGHT] ? 0x2000 : 0;
        held |= keys[SDL_SCANCODE_UP] ? 0x1000 : 0;
        held |= keys[SDL_SCANCODE_RETURN] ? 0x0800 : 0;
        held |= keys[SDL_SCANCODE_RSHIFT] ? 0x0100 : 0;
        held |= keys[SDL_SCANCODE_S] ? 0x0080 : 0;
        held |= keys[SDL_SCANCODE_X] ? 0x0040 : 0;
        held |= keys[SDL_SCANCODE_D] ? 0x0020 : 0;
        held |= keys[SDL_SCANCODE_W] ? 0x0010 : 0;
        return static_cast<u16>(~held);
    }

private:
    u16 mRepeated = 0;
    u32 mPolls = 0;
    std::string mTitle;
    std::chrono::steady_clock::time_point mRateStart;
    u32 mRateFields = 0;
    SDL_Window* mWindow = nullptr;
    SDL_Renderer* mRenderer = nullptr;
    SDL_Texture* mTexture = nullptr;
    int mWidth = 0;
    int mHeight = 0;
    bool mResized = false;
    bool mClosing = false;
};

// The registers are kept here because the unit reads them while it draws.
class Graphics : public uee::GsHandler {
public:
    Graphics(GsLibrary& gs, Window& window, int renderer, int threads,
             std::string resources, std::string frameDirectory)
        : mGs(gs), mWindow(window), mRenderer(renderer), mThreads(threads),
          mResources(std::move(resources)), mFrameDirectory(std::move(frameDirectory)) {}

    ~Graphics() override {
        if (mOpen) {
            mGs.closeRenderer();
        }
    }

    void begin(std::span<const u8> registers) override {
        mRegisters.assign(registers.begin(), registers.end());
        // The software renderer hands its picture back and this shows it; a
        // hardware one is given the window and draws into it.
        const BorrowedWindow window = mWindow.getBorrowedWindow();
        const bool presents = mRenderer != 0;
        mOpen = mGs.open(mRegisters.data(), mRenderer, presents ? &window : nullptr,
                         mResources.empty() ? nullptr : mResources.c_str(), mThreads) != 0;
        mPresents = mOpen && presents;
        if (!mOpen) {
            logger.fatal("the renderer did not open");
        }
    }

    void transfer(u32 path, std::span<const u8> data) override {
        mGs.transfer(static_cast<int>(path), data.data(), static_cast<u32>(data.size()));
    }

    u32 getFieldCount() const { return mFields; }
    void setFieldHook(std::function<void(u32)> hook) { mFieldHook = std::move(hook); }

    void vsync(u32 field, std::span<const u8> registers) override {
        std::copy(registers.begin(), registers.end(), mRegisters.begin());
        mGs.endField(static_cast<int>(field));

        int width = 0;
        int height = 0;
        if (!mPresents && mGs.readDisplay(mPixels.data(), static_cast<int>(mPixels.size()),
                                          &width, &height) != 0) {
            mWindow.show(mPixels.data(), width, height);
        }
        if (!mFrameDirectory.empty()) {
            mGs.writeDisplay(
                std::format("{}/field{:05d}.png", mFrameDirectory, mFields).c_str());
        }
        ++mFields;
        mWindow.countField();
        if (mFieldHook) {
            mFieldHook(mFields);
        }

        mWindow.pollEvents();
        if (mWindow.takeResize()) {
            const BorrowedWindow window = mWindow.getBorrowedWindow();
            mGs.resize(window.width, window.height, window.scale);
        }
    }

private:
    GsLibrary& mGs;
    Window& mWindow;
    int mRenderer = 0;
    int mThreads = 0;
    bool mPresents = false;
    std::string mResources;
    std::string mFrameDirectory;
    u32 mFields = 0;
    std::function<void(u32)> mFieldHook;
    std::vector<u8> mRegisters;
    std::vector<u32> mPixels = std::vector<u32>(kMaxWidth * kMaxHeight);
    bool mOpen = false;
};

}  // namespace

int main(int argc, char** argv) {
    uee::configureLogging();
    if (argc < 2) {
        logger.fatal("usage: {} <elf> [disc.iso] [patches.toml]", argv[0]);
        return 2;
    }

    // 0 software, 1 whatever the platform prefers, then opengl, vulkan, d3d11,
    // d3d12, metal, as the library numbers them.
    int renderer = 0;
    if (const char* name = std::getenv("UEE_GS_RENDERER")) {
        const std::string wanted = name;
        const std::array<std::string, 7> names{"sw", "auto", "ogl", "vk", "d3d11", "d3d12",
                                               "metal"};
        const auto found = std::find(names.begin(), names.end(), wanted);
        if (found == names.end()) {
            logger.fatal("no renderer is called {}", wanted);
            return 2;
        }
        renderer = static_cast<int>(found - names.begin());
    }

    const char* library = std::getenv("UEE_PCSX2HOOK");
    GsLibrary gs;
    if (!gs.load(library == nullptr ? "libpcsx2hook.so" : library)) {
        return 2;
    }

    uee::GameConfig config;
    if (argc > 3) {
        config = uee::loadGameConfig(argv[3]);
    }

    uee::Memory memory = uee::makeMainMemory();
    uee::Interpreter interpreter(memory);
    uee::Kernel kernel(interpreter.ee, memory, config);
    interpreter.ee.cpu.pc = uee::loadElf(readFile(argv[1]), memory).entry;
    uee::applyPatches(memory, config.patches);

    const std::unique_ptr<uee::Disc> disc = uee::openDisc(argc > 2 ? argv[2] : "");
    if (argc > 2) {
        if (!disc->isOpen()) {
            logger.fatal("cannot open disc {}", argv[2]);
            return 2;
        }
        kernel.attachDisc(*disc);
    }

    Window window;
    if (!window.open(std::filesystem::path(argv[1]).filename().string())) {
        return 2;
    }

    const char* frames = std::getenv("UEE_GS_FRAMES");
    const char* resources = std::getenv("UEE_PCSX2_RESOURCES");
    // Threads the software renderer rasterizes on besides the one that emulates;
    // with none, drawing shares the emulation thread. PCSX2's default is 2.
    int threads = 2;
    if (const char* count = std::getenv("UEE_GS_THREADS")) {
        threads = std::atoi(count);
    }
    Graphics graphics(gs, window, renderer, threads, resources == nullptr ? "" : resources,
                      frames == nullptr ? "" : frames);
    std::optional<uee::GsCaptureWriter> capture;
    std::optional<SplitGs> both;
    if (const char* path = std::getenv("UEE_GS_CAPTURE")) {
        capture.emplace(path, std::filesystem::path(argv[1]).stem().string());
        if (!capture->isOpen()) {
            logger.fatal("cannot write the graphics stream to {}", path);
            return 2;
        }
        both.emplace(graphics, *capture);
    }
    kernel.attachGsHandler(both ? static_cast<uee::GsHandler*>(&*both) : &graphics);
    if (const char* buttons = std::getenv("UEE_PAD")) {
        window.pressRepeatedly(static_cast<u16>(std::strtoul(buttons, nullptr, 0)));
    }
    kernel.attachPad(&window);

    std::optional<uee::CardFile> card;
    if (const char* path = std::getenv("UEE_CARD")) {
        card.emplace(path);
        kernel.attachCard(&*card);
    }

    // Every run adds the micro programs it reached to the manifest the build
    // translates, which by default sits beside the game's executable.
    const char* manifestPath = std::getenv("UEE_VU_MANIFEST");
    const std::filesystem::path vuManifest =
        manifestPath != nullptr ? std::filesystem::path(manifestPath)
                                : std::filesystem::path(argv[1]).parent_path() / "vu_programs.txt";
    uee::VuProgramLog vuLog;
    vuLog.load(vuManifest);
    const bool vuInterpreted = std::getenv("UEE_VU_INTERPRET") != nullptr;
    // Speed over exactness: host float arithmetic in translated programs.
    const bool vuFast = std::getenv("UEE_VU_FAST") != nullptr;
    // The runs of a stretch of fields, whole, for bench_vu to replay.
    const char* capturePath = std::getenv("UEE_VU_CAPTURE");
    std::optional<uee::VuCapture> vuCapture;
    u32 captureFirst = 0;
    u32 captureCount = 0;
    if (capturePath != nullptr) {
        const char* fields = std::getenv("UEE_VU_CAPTURE_FIELDS");
        if (fields == nullptr || std::sscanf(fields, "%u,%u", &captureFirst, &captureCount) != 2) {
            logger.fatal("UEE_VU_CAPTURE needs UEE_VU_CAPTURE_FIELDS=<first>,<count>");
            return 2;
        }
        vuCapture.emplace();
        graphics.setFieldHook([&](u32 field) {
            if (field == captureFirst) {
                vuCapture->setRecording(true);
            } else if (field == captureFirst + captureCount && vuCapture->isRecording()) {
                vuCapture->setRecording(false);
                vuCapture->save(capturePath);
                logger.info("wrote {} VU runs to {}", vuCapture->getRuns().size(), capturePath);
            }
        });
    }
    std::span<const uee::TranslatedVuProgram> vuTranslated;
    if (!vuInterpreted) {
        vuTranslated = getTranslatedVuPrograms(vuFast);
    }
    uee::VuPrograms vuPrograms{vuTranslated, &vuLog, vuCapture ? &*vuCapture : nullptr,
                               std::getenv("UEE_VU_CHECK") != nullptr};
    kernel.attachVuPrograms(&vuPrograms);

    // Without a sink the game's sceMpegGetPicture drives an IPU that cannot
    // decode; dropping the stream answers the call.
    DroppedVideo video;
    uee::Movie movie{kernel.mmio().ipu(), config.mpegGetPicture};
    movie.attach(&video);
    uee::Hook* hooks[] = {&movie};

    // Running the same build both ways is what says whether a fault is in the
    // translation or in what it calls.
    const uee::FunctionRunner functions =
        std::getenv("UEE_INTERPRET") != nullptr ? nullptr : kTranslated;
    uee::Machine machine{interpreter, memory, kernel.mmio(), kernel, kernel.budget(),
                         functions, hooks};
    machine.stopRequest = &window;

    std::optional<std::chrono::milliseconds> limit;
    if (const char* seconds = std::getenv("UEE_SECONDS")) {
        limit = std::chrono::seconds(std::strtoull(seconds, nullptr, 0));
    }

    const uee::Stop stop = uee::run(machine, limit);
    logger.info("{} after {} fields", uee::describe(stop), graphics.getFieldCount());
    vuLog.save(vuManifest);
    return 0;
}
