#include "log.h"
#include "elf.h"
#include "patches.h"
#include "memory.h"
#include "symbols.h"
#include "ee_translate.h"
#include "types.h"

#include <spdlog/spdlog.h>

#include <charconv>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
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

// Any "name = 0xADDR" line, so the file a frontend already keeps its addresses
// in is the one read here and there is no second copy to go stale.
std::set<u32> readAddresses(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        spdlog::critical("cannot open {}", path.string());
        std::exit(2);
    }
    std::set<u32> addresses;
    std::string line;
    while (std::getline(file, line)) {
        const usize assign = line.find('=');
        const usize hex = line.find("0x", assign == std::string::npos ? 0 : assign);
        if (assign == std::string::npos || hex == std::string::npos ||
            line.find('#') < assign) {
            continue;
        }
        u32 address = 0;
        const char* first = line.data() + hex + 2;
        const auto [end, error] = std::from_chars(first, line.data() + line.size(), address, 16);
        if (error == std::errc{}) {
            addresses.insert(address);
        }
    }
    return addresses;
}

}  // namespace

// The count comes in so a build system can name the units up front.
int main(int argc, char** argv) {
    uee::configureLogging();
    if (argc < 4) {
        spdlog::error("usage: {} <test.elf> <out-stem> <units> [patches.toml] [addresses.toml]",
                      argv[0]);
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

    // Translating what the patches leave behind, because that is what runs.
    std::optional<u32> gp;
    std::vector<std::pair<u32, u32>> readOnlySegments;
    if (argc > 4) {
        const uee::GameConfig config = uee::loadGameConfig(argv[4]);
        uee::applyPatches(memory, config.patches);
        gp = config.gp;
        readOnlySegments = config.readOnlySegments;
    }

    // A map beside the program names its functions, which is what the generated
    // code is labelled with.
    const std::filesystem::path elf = argv[1];
    uee::Symbols symbols;
    symbols.setSearchPath(elf.parent_path().string());
    symbols.load(elf.filename().string(), 0, 0xFFFFFFFFu);

    // A frontend that reimplements a function wants the guest body skipped, and
    // asking costs a branch, so only the addresses it names get asked about.
    std::set<u32> hookable;
    if (argc > 5) {
        hookable = readAddresses(argv[5]);
        spdlog::info("{} addresses may be hooked", hookable.size());
    }

    const std::filesystem::path declarations = stem.string() + ".decls.h";
    const uee::TranslatedSource source =
        uee::translate(memory, program, units, declarations.filename().string(), &symbols,
                       hookable, gp, readOnlySegments);

    writeFile(declarations, source.declarations);
    for (usize i = 0; i < units; ++i) {
        writeFile(std::format("{}.{}.cpp", stem.string(), i),
                  i < source.units.size() ? source.units[i]
                                          : std::string{"namespace uee {}\n"});
    }
    return 0;
}
