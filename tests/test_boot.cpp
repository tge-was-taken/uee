#include "elf.h"
#include "interpreter.h"
#include "kernel.h"
#include "log.h"
#include "memory.h"
#include "run.h"
#include "types.h"

#include <charconv>
#include <chrono>
#include <format>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <string_view>
#include <vector>

namespace {

uee::LoggerChannel log("boot");

// Only a bound on a guest that never reaches its checkpoint.
constexpr std::chrono::seconds kTimeLimit{300};

struct Options {
    std::filesystem::path elf;
    std::filesystem::path files;
    std::string checkpoint;
    u64 entries = 0;
};

[[noreturn]] void fail(const std::string& message) {
    log.fatal("{}", message);
    std::exit(2);
}

std::vector<u8> readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        fail(std::format("cannot open {}", path.string()));
    }
    return std::vector<u8>(std::istreambuf_iterator<char>(file), {});
}

u64 parseCount(std::string_view text) {
    u64 value = 0;
    if (std::from_chars(text.data(), text.data() + text.size(), value).ec != std::errc{}) {
        fail(std::format("{} is not a count", text));
    }
    return value;
}

Options parseArguments(int argc, char** argv) {
    if (argc < 2) {
        fail(std::format("usage: {} <elf> [--files <dir>] [--enter <symbol> <count>]", argv[0]));
    }
    Options options{argv[1], {}, {}, 0};
    for (int i = 2; i < argc;) {
        const std::string_view flag = argv[i];
        if (flag == "--files" && i + 1 < argc) {
            options.files = argv[i + 1];
            i += 2;
        } else if (flag == "--enter" && i + 2 < argc) {
            options.checkpoint = argv[i + 1];
            options.entries = parseCount(argv[i + 2]);
            i += 3;
        } else {
            fail(std::format("unexpected argument {}", flag));
        }
    }
    return options;
}

// A guest stops on its checkpoint or returns from main. Every other reason,
// running out of time included, is the runtime failing to keep up with it.
bool isClean(uee::StopReason reason) {
    return reason == uee::StopReason::Stopped || reason == uee::StopReason::Exited;
}

std::chrono::milliseconds timeLimit() {
    const char* seconds = std::getenv("UEE_SECONDS");
    return seconds != nullptr ? std::chrono::milliseconds(std::strtoull(seconds, nullptr, 0) * 1000)
                              : std::chrono::duration_cast<std::chrono::milliseconds>(kTimeLimit);
}

// Ending on the checkpoint rather than on the clock keeps what the test covers
// the same however fast the host is.
class UntilCheckpoint : public uee::StopRequest {
public:
    UntilCheckpoint(const uee::SymbolWatch& hooks, std::string checkpoint, u64 entries)
        : mSymbolWatch(hooks), mCheckpoint(std::move(checkpoint)), mEntries(entries) {}

    bool isStopRequested() override { return mSymbolWatch.getHits(mCheckpoint) >= mEntries; }

private:
    const uee::SymbolWatch& mSymbolWatch;
    std::string mCheckpoint;
    u64 mEntries;
};

}  // namespace

int main(int argc, char** argv) {
    uee::configureLogging();
    const Options options = parseArguments(argc, argv);

    uee::Memory memory = uee::makeMainMemory();
    uee::Interpreter interpreter(memory);
    uee::Kernel kernel(interpreter.ee, memory);
    interpreter.ee.cpu.pc = uee::loadElf(readFile(options.elf), memory).entry;

    if (!options.files.empty()) {
        if (!std::filesystem::is_directory(options.files)) {
            fail(std::format("{} is not a directory of guest files", options.files.string()));
        }
        kernel.attachFiles(options.files);
    }

    kernel.symbols().setSearchPath(options.elf.parent_path().string());
    kernel.symbols().load(options.elf.filename().string(), 0, 0xFFFFFFFFu);
    if (!options.checkpoint.empty()) {
        kernel.symbolWatch().watch(kernel.symbols(), options.checkpoint);
        if (kernel.symbolWatch().empty()) {
            fail(std::format("no symbol named {} in {}", options.checkpoint,
                             options.elf.string()));
        }
    }

    UntilCheckpoint until{kernel.symbolWatch(), options.checkpoint, options.entries};
    uee::Machine machine{.interpreter = interpreter,
                         .memory = memory,
                         .mmio = kernel.mmio(),
                         .kernel = kernel,
                         .budget = kernel.budget(),
                         .stopRequest = options.checkpoint.empty() ? nullptr : &until};
    const uee::Stop stop = uee::run(machine, timeLimit());
    const u64 reached = kernel.symbolWatch().getHits(options.checkpoint);

    if (isClean(stop.reason) && reached >= options.entries) {
        log.info("{}, {} entered {} times", uee::describe(stop), options.checkpoint, reached);
        return 0;
    }

    if (!isClean(stop.reason)) {
        log.error("{}", uee::describe(stop));
    }
    if (reached < options.entries) {
        log.error("{} entered {} times, wanted {}", options.checkpoint, reached,
                  options.entries);
    }
    std::fputs(kernel.readOutput().c_str(), stdout);
    kernel.reportThreads();
    return 1;
}
