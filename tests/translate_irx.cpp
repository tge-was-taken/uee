#include "log.h"
#include "iop_run.h"
#include "iop_translate.h"
#include "irx.h"
#include "memory.h"
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
        spdlog::error("usage: {} <test.irx> <out.cpp>", argv[0]);
        return 2;
    }

    uee::Memory memory = uee::makeIopMemory();
    std::vector<uee::Import> imports;
    const uee::IrxModule module =
        uee::loadIrx(readFile(argv[1]), memory, uee::kIopModuleBase, {}, imports);
    const std::string source = uee::translateIop(memory, module.program);

    std::ofstream out(argv[2]);
    if (!out) {
        spdlog::critical("cannot write {}", argv[2]);
        return 2;
    }
    out << source;
    return 0;
}
