#include "log.h"
#include "elf.h"
#include "memory.h"
#include "translate.h"
#include "types.h"

#include <spdlog/spdlog.h>

#include <charconv>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::vector<u8> readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        spdlog::critical("cannot open {}", path.string());
        std::exit(2);
    }
    return std::vector<u8>(std::istreambuf_iterator<char>(file), {});
}

void writeFile(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path);
    if (!out) {
        spdlog::critical("cannot write {}", path.string());
        std::exit(2);
    }
    out << text;
}

}  // namespace

// The count comes in so a build system can name the units up front.
int main(int argc, char** argv) {
    uee::configureLogging();
    if (argc < 4) {
        spdlog::error("usage: {} <test.elf> <out-stem> <units>", argv[0]);
        return 2;
    }

    const std::filesystem::path stem = argv[2];
    usize units = 0;
    const std::string_view count = argv[3];
    if (std::from_chars(count.begin(), count.end(), units).ec != std::errc{} || units == 0) {
        spdlog::critical("{} is not a unit count", count);
        return 2;
    }

    uee::Memory memory = uee::makeMainMemory();
    const uee::Program program = uee::loadElf(readFile(argv[1]), memory);
    const std::filesystem::path declarations = stem.string() + ".decls.h";
    const uee::TranslatedSource source =
        uee::translate(memory, program, units, declarations.filename().string());

    writeFile(declarations, source.declarations);
    for (usize i = 0; i < units; ++i) {
        writeFile(std::format("{}.{}.cpp", stem.string(), i),
                  i < source.units.size() ? source.units[i]
                                          : std::string{"namespace uee {}\n"});
    }
    return 0;
}
