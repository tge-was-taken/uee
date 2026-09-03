#include "elf.h"
#include "interpreter.h"
#include "disc.h"
#include "iop_translate.h"
#include "kernel.h"
#include "patches.h"
#include "log.h"
#include "memory.h"
#include "run.h"
#include "types.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdio>
#include <print>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace uee {
bool runFunction(Machine& machine, u32 pc);
}

namespace {

constexpr u64 kDefaultLimit = 200'000'000;

std::vector<u8> readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        spdlog::critical("cannot open {}", path.string());
        std::exit(2);
    }
    return std::vector<u8>(std::istreambuf_iterator<char>(file), {});
}

u64 instructionLimit() {
    const char* limit = std::getenv("UEE_LIMIT");
    return limit != nullptr ? std::strtoull(limit, nullptr, 0) : kDefaultLimit;
}

}  // namespace

// Same program, same budget, so the only thing that moves between runs is how
// long the host took to get through it.
#ifdef UEE_IOP_MODULES
namespace uee {
UEE_IOP_MODULE_DECLARATIONS
}  // namespace uee
#endif

// What the build translated alongside the program. Each is only correct at the
// address it was translated for, which Iop checks when the image loads.
std::span<uee::TranslatedIrx* const> translatedIopModules() {
#ifdef UEE_IOP_MODULES
    static uee::TranslatedIrx* const kModules[] = {UEE_IOP_MODULE_LIST};
    return kModules;
#else
    return {};
#endif
}

int main(int argc, char** argv) {
    uee::configureLogging();
    if (argc < 2) {
        spdlog::error("usage: {} <program.elf> [--interpret] [--section <n>] [--files <dir>] [--disc <iso>] [--patches <toml>]",
                      argv[0]);
        return 2;
    }
    bool interpret = false;
    u32 section = 0;
    std::filesystem::path files;
    std::filesystem::path disc;
    std::filesystem::path patches;
    for (int i = 2; i < argc; ++i) {
        const std::string_view flag = argv[i];
        if (flag == "--interpret") {
            interpret = true;
        } else if (flag == "--section" && i + 1 < argc) {
            section = static_cast<u32>(std::strtoul(argv[++i], nullptr, 0));
        } else if (flag == "--files" && i + 1 < argc) {
            files = argv[++i];
        } else if (flag == "--disc" && i + 1 < argc) {
            disc = argv[++i];
        } else if (flag == "--patches" && i + 1 < argc) {
            patches = argv[++i];

        } else {
            spdlog::error("unexpected argument {}", flag);
            return 2;
        }
    }

    uee::Memory memory = uee::makeMainMemory();
    uee::Interpreter interpreter(memory);
    const uee::GameConfig config =
        patches.empty() ? uee::GameConfig{} : uee::loadGameConfig(patches.string());
    uee::Kernel kernel(interpreter.ee, memory, config, translatedIopModules());
    if (!files.empty()) {
        kernel.attachFiles(files);
    }
    uee::Disc image{disc.string()};
    if (!disc.empty()) {
        if (!image.isOpen()) {
            spdlog::error("cannot open disc {}", disc.string());
            return 2;
        }
        kernel.attachDisc(image);
    }
    interpreter.ee.cpu.pc = uee::loadElf(readFile(argv[1]), memory).entry;
    uee::applyPatches(memory, config.patches);

    if (section != 0) {
        const std::filesystem::path elf = argv[1];
        kernel.symbols().setSearchPath(elf.parent_path().string());
        kernel.symbols().load(elf.filename().string(), 0, 0xFFFFFFFFu);
        const std::optional<u32> selector = kernel.symbols().find("gSection");
        if (!selector) {
            spdlog::error("{} has no gSection to select a section with", argv[1]);
            return 2;
        }
        memory.write32(*selector, section);
    }

    uee::Machine machine{interpreter, memory, kernel.mmio(), kernel, kernel.budget(),
                         interpret ? nullptr : uee::runFunction};
    const auto started = std::chrono::steady_clock::now();
    kernel.stopAfter(instructionLimit());
    const uee::Stop stop = uee::run(machine);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    const double seconds = std::chrono::duration<double>(elapsed).count();
    const u64 executed = kernel.executed();
    std::fputs(kernel.readOutput().c_str(), stdout);
    std::print(stderr, "{}\n{} instructions in {:.3f}s, {:.1f} million per second\n",
               uee::describe(stop), executed, seconds, executed / seconds / 1e6);
    return 0;
}
