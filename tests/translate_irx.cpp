#include "log.h"
#include "iop_run.h"
#include "iop_translate.h"
#include "irx.h"
#include "memory.h"
#include "types.h"

#include <spdlog/spdlog.h>

#include <charconv>
#include <cstdlib>
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

usize parseCount(std::string_view text) {
    usize value = 0;
    if (std::from_chars(text.begin(), text.end(), value).ec != std::errc{} || value == 0) {
        spdlog::critical("{} is not a unit count", text);
        std::exit(2);
    }
    return value;
}

}  // namespace

int main(int argc, char** argv) {
    uee::configureLogging();
    if (argc < 5) {
        spdlog::error("usage: {} <test.irx> <out-stem> <name> <units> [base]", argv[0]);
        return 2;
    }

    const std::filesystem::path stem = argv[2];
    const usize units = parseCount(argv[4]);
    const u32 base = argc > 5 ? static_cast<u32>(std::strtoul(argv[5], nullptr, 0))
                              : uee::kIopModuleBase;

    uee::Memory memory = uee::makeIopMemory();
    std::vector<uee::Import> imports;
    const uee::IrxModule module = uee::loadIrx(readFile(argv[1]), memory, base, {}, imports);
    const std::filesystem::path declarations = stem.string() + ".decls.h";
    const uee::TranslatedSource source = uee::translateIop(
        memory, module.program, argv[3], imports, units, declarations.filename().string());

    writeFile(declarations, source.declarations);
    for (usize i = 0; i < units; ++i) {
        writeFile(std::format("{}.{}.cpp", stem.string(), i),
                  i < source.units.size() ? source.units[i]
                                          : std::string{"namespace uee {}\n"});
    }
    return 0;
}
