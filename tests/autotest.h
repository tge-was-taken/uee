#pragma once

#include "log.h"
#include "elf.h"
#include "interpreter.h"
#include "kernel.h"
#include "memory.h"
#include "run.h"
#include "ee_translate.h"
#include "types.h"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <chrono>
#include <vector>

namespace uee::autotest {

// Only a bound on a guest that never finishes: one that does prints and exits.
constexpr std::chrono::seconds kTimeLimit{120};
constexpr usize kReportedDifferences = 12;

inline std::vector<u8> readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error(std::format("cannot open {}", path.string()));
    }
    return std::vector<u8>(std::istreambuf_iterator<char>(file), {});
}

inline std::vector<std::string> readLines(std::string_view text) {
    std::vector<std::string> lines;
    usize start = 0;
    while (start <= text.size()) {
        const usize end = text.find('\n', start);
        std::string line(text.substr(start, end - start));
        std::erase(line, '\r');
        lines.push_back(std::move(line));
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return lines;
}

inline void reportDifferences(std::string_view name, std::string_view actual,
                              std::string_view expected) {
    const std::vector<std::string> actualLines = readLines(actual);
    const std::vector<std::string> expectedLines = readLines(expected);
    spdlog::error("{}: {} lines produced, {} expected", name, actualLines.size(),
                  expectedLines.size());

    usize reported = 0;
    for (usize i = 0; i < std::max(actualLines.size(), expectedLines.size()); ++i) {
        const std::string_view mine =
            i < actualLines.size() ? std::string_view(actualLines[i]) : "<missing>";
        const std::string_view theirs =
            i < expectedLines.size() ? std::string_view(expectedLines[i]) : "<missing>";
        if (mine == theirs) {
            continue;
        }
        if (++reported > kReportedDifferences) {
            spdlog::error("  ... more differences follow");
            return;
        }
        spdlog::error("  line {} expected: {}", i + 1, theirs);
        spdlog::error("  line {} actual:   {}", i + 1, mine);
    }
}

inline int run(int argc, char** argv, FunctionRunner translated) {
    configureLogging();
    if (argc < 3) {
        spdlog::error("usage: {} <test.elf> <test.expected>", argv[0]);
        return 2;
    }

    const std::filesystem::path elfPath = argv[1];
    const std::string name = elfPath.filename().string();

    Memory memory = makeMainMemory();
    Interpreter interpreter(memory);
    Kernel kernel(interpreter.ee, memory);

    interpreter.ee.cpu.pc = loadElf(readFile(elfPath), memory).entry;

    Machine machine{interpreter, memory, kernel.mmio(), kernel, kernel.budget(), translated};
    const Stop stop = run(machine, kTimeLimit);
    const std::string& actual = kernel.readOutput();
    const std::vector<u8> expectedBytes = readFile(argv[2]);
    const std::string expected(expectedBytes.begin(), expectedBytes.end());

    if (stop.reason != StopReason::Exited) {
        spdlog::error("{}: {}", name, describe(stop));
        reportDifferences(name, actual, expected);
        return 1;
    }

    const std::vector<std::string> actualLines = readLines(actual);
    const std::vector<std::string> expectedLines = readLines(expected);
    if (actualLines != expectedLines) {
        reportDifferences(name, actual, expected);
        return 1;
    }

    return 0;
}

}  // namespace uee::autotest
