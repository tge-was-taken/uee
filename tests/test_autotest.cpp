#include "elf.h"
#include "interpreter.h"
#include "kernel.h"
#include "memory.h"
#include "run.h"
#include "types.h"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr u64 kInstructionLimit = 500'000'000;

std::vector<u8> readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error(std::format("cannot open {}", path.string()));
    }
    return std::vector<u8>(std::istreambuf_iterator<char>(file), {});
}

std::string readText(const std::filesystem::path& path) {
    const std::vector<u8> bytes = readFile(path);
    std::string text(bytes.begin(), bytes.end());
    std::erase(text, '\r');
    return text;
}

usize findFirstDifference(std::string_view actual, std::string_view expected) {
    const usize shared = std::min(actual.size(), expected.size());
    for (usize i = 0; i < shared; ++i) {
        if (actual[i] != expected[i]) {
            return i;
        }
    }
    return shared;
}

std::string quoteLineAt(std::string_view text, usize offset) {
    const usize start = text.rfind('\n', offset);
    const usize end = text.find('\n', offset);
    return std::string(text.substr(start == std::string_view::npos ? 0 : start + 1,
                                   end == std::string_view::npos ? end : end - start - 1));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        spdlog::error("usage: {} <test.elf> <test.expected>", argv[0]);
        return 2;
    }

    const std::filesystem::path elfPath = argv[1];
    const std::filesystem::path expectedPath = argv[2];

    uee::Memory memory = uee::makeMainMemory();
    uee::Interpreter interpreter(memory);
    uee::Kernel kernel(interpreter.machine, memory);

    interpreter.machine.cpu.pc = uee::loadElf(readFile(elfPath), memory);

    const uee::Stop stop = uee::run(interpreter, memory, kernel, kInstructionLimit);
    const std::string& actual = kernel.readOutput();
    const std::string expected = readText(expectedPath);

    if (stop.reason != uee::StopReason::Exited) {
        spdlog::error("{}: {}", elfPath.filename().string(), uee::describe(stop));
        spdlog::error("produced {} of {} expected bytes", actual.size(), expected.size());
        return 1;
    }

    if (actual != expected) {
        const usize offset = findFirstDifference(actual, expected);
        spdlog::error("{}: output differs at byte {}", elfPath.filename().string(), offset);
        spdlog::error("expected: {}", quoteLineAt(expected, offset));
        spdlog::error("actual:   {}", quoteLineAt(actual, offset));
        return 1;
    }

    return 0;
}
