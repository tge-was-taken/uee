#include "log.h"
#include "elf.h"
#include "memory.h"
#include "translate.h"
#include "types.h"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
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

}  // namespace

int main(int argc, char** argv) {
    uee::configureLogging();
    if (argc < 3) {
        spdlog::error("usage: {} <test.elf> <out.cpp>", argv[0]);
        return 2;
    }

    uee::Memory memory = uee::makeMainMemory();
    const uee::Program program = uee::loadElf(readFile(argv[1]), memory);
    const std::string source = uee::translate(memory, program);

    std::ofstream out(argv[2]);
    if (!out) {
        spdlog::critical("cannot write {}", argv[2]);
        return 2;
    }
    out << source;
    return 0;
}
