#include "disc.h"
#include "elf.h"
#include "log.h"
#include "patches.h"
#include "interpreter.h"
#include "kernel.h"
#include "memory.h"
#include "run.h"

#include <cstdio>
#include <format>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
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

    if (const char* hooks = std::getenv("UEE_HOOKS")) {
        kernel.hooks().watch(kernel.symbols(), hooks);
    }

    logging = &kernel;

    uee::Machine machine{interpreter, memory, kernel, nullptr};
    const char* limit = std::getenv("UEE_LIMIT");
    const uee::Stop stop = uee::run(machine, limit != nullptr ? std::strtoull(limit, nullptr, 0)
                                                             : 2'000'000'000);

    std::fputs(kernel.readOutput().c_str(), stdout);
    if (stop.reason == uee::StopReason::Exited) {
        log.info("{}", uee::describe(stop));
        return 0;
    }
    log.error("{}", uee::describe(stop));
    kernel.reportThreads();
    return 1;
}
