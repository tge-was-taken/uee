#include "disc.h"
#include "elf.h"
#include "log.h"
#include "patches.h"
#include "interpreter.h"
#include "kernel.h"
#include "memory.h"
#include "movie.h"
#include "run.h"
#include "card_file.h"
#include "wav_sink.h"

#include <chrono>
#include <cstdio>
#include <format>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>


namespace {
uee::LoggerChannel log("run");
}  // namespace

int main(int argc, char** argv) {
    uee::Kernel* logging = nullptr;
    uee::configureLogging([&logging] {
        return logging == nullptr ? std::string{"-"}
                                  : std::format("ee{}", logging->currentThread());
    });
    if (argc < 2) {
        log.fatal("usage: run_guest <elf> [disc.iso] [patches.toml]");
        return 2;
    }

    std::ifstream file(argv[1], std::ios::binary);
    if (!file) {
        log.fatal("cannot open {}", argv[1]);
        return 2;
    }
    const std::vector<u8> image{std::istreambuf_iterator<char>(file),
                                std::istreambuf_iterator<char>()};

    uee::GameConfig config;
    if (argc > 3) {
        try {
            config = uee::loadGameConfig(argv[3]);
        } catch (const std::exception& error) {
            log.fatal("{}", error.what());
            return 2;
        }
    }

    uee::Memory memory = uee::makeMainMemory();
    uee::Interpreter interpreter(memory);
    uee::Kernel kernel(interpreter.ee, memory, config);
    interpreter.ee.cpu.pc = uee::loadElf(image, memory).entry;
    uee::applyPatches(memory, config.patches);

    // Resident before the game asks, so the reboot needs no modelling.
    const std::filesystem::path root =
        argc > 3 ? std::filesystem::path(argv[3]).parent_path() : std::filesystem::path();
    for (const std::string& module : config.preload) {
        const std::filesystem::path path = root / module;
        std::ifstream irx(path, std::ios::binary);
        if (!irx) {
            log.fatal("cannot open preloaded module {}", path.string());
            return 2;
        }
        const std::vector<u8> bytes{std::istreambuf_iterator<char>(irx),
                                    std::istreambuf_iterator<char>()};
        if (!kernel.iop().loadModule(path.stem().string(), bytes)) {
            log.fatal("preloaded module {} did not start", path.string());
            return 2;
        }
        log.info("preloaded {}", path.stem().string());
    }

    uee::Disc disc{argc > 2 ? argv[2] : ""};
    if (disc.isOpen()) {
        kernel.attachDisc(disc);
    } else if (argc > 2) {
        log.fatal("cannot open disc {}", argv[2]);
        return 2;
    }

    if (const char* files = std::getenv("UEE_FILES")) {
        kernel.attachFiles(files);
    }

    if (const char* maps = std::getenv("UEE_SYMBOLS")) {
        kernel.symbols().setSearchPath(maps);
        kernel.iopSymbols().setSearchPath(maps);
        kernel.symbols().load(std::filesystem::path(argv[1]).filename().string(), 0,
                              0xFFFFFFFFu);
    }

    std::optional<uee::WavSink> audio;
    if (const char* path = std::getenv("UEE_AUDIO")) {
        audio.emplace(path, uee::kSpuSampleRate, uee::kSpuChannels);
        if (!audio->isOpen()) {
            log.fatal("cannot write audio to {}", path);
            return 2;
        }
        kernel.attachAudio(&*audio);
        log.info("writing audio to {}", path);
    }

    // A held mask, as libpad reports it: SCE_PADstart is 0x0800. A menu acts on
    // the press rather than the level, so the mask is let go between presses.
    struct PressedPad : uee::PadSource {
        u16 held = 0;
        u32 polls = 0;

        u16 buttons() override {
            const bool down = polls++ / 20 % 2 == 0;
            return static_cast<u16>(~(down ? held : 0));
        }
    } pad;
    if (const char* held = std::getenv("UEE_PAD")) {
        pad.held = static_cast<u16>(std::strtoul(held, nullptr, 0));
        log.info("pressing pad buttons {:04x}", pad.held);
    }
    kernel.attachPad(&pad);

    // Nothing here displays a movie, so the SFD player runs its state machine
    // out on its own rather than being patched.
    struct NoVideo : uee::VideoDecoder {
        void feed(std::span<const u8>) override {}
        std::optional<uee::Picture> takePicture() override { return std::nullopt; }
    } video;
    uee::Movie movie{memory, kernel.mmio().ipu(), config.mpegGetPicture};
    movie.attach(&video);
    uee::Hook* hooks[] = {&movie};

    std::optional<uee::CardFile> card;
    if (const char* path = std::getenv("UEE_CARD")) {
        card.emplace(path);
        if (!card->isOpen()) {
            log.fatal("cannot open memory card image {}", path);
            return 2;
        }
        kernel.attachCard(&*card);
        log.info("memory card image {}", path);
    }

    if (const char* hooks = std::getenv("UEE_HOOKS")) {
        kernel.hooks().watch(kernel.symbols(), hooks);
    }

    logging = &kernel;

    uee::Machine machine{interpreter, memory, kernel, nullptr, hooks};
    std::optional<std::chrono::milliseconds> limit;
    if (const char* seconds = std::getenv("UEE_SECONDS")) {
        limit = std::chrono::seconds(std::strtoull(seconds, nullptr, 0));
    }
    const uee::Stop stop = uee::run(machine, limit);

    std::fputs(kernel.readOutput().c_str(), stdout);
    if (stop.reason == uee::StopReason::Exited) {
        log.info("{}", uee::describe(stop));
        return 0;
    }
    log.error("{}", uee::describe(stop));
    kernel.reportThreads();
    return 1;
}
