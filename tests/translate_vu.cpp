#include "vu_programs.h"
#include "vu_translate.h"

#include <spdlog/spdlog.h>

#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>

namespace {

void writeFile(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path);
    if (!out) {
        spdlog::critical("cannot write {}", path.string());
        std::exit(2);
    }
    out << text;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        spdlog::error("usage: {} <vu-manifest> <out-stem> <units>", argv[0]);
        return 2;
    }
    const std::string_view unitsText = argv[3];
    usize units = 0;
    const auto [end, error] =
        std::from_chars(unitsText.data(), unitsText.data() + unitsText.size(), units);
    if (error != std::errc{} || end != unitsText.data() + unitsText.size() || units < 2) {
        spdlog::error("units must be a number of at least 2, not '{}'", unitsText);
        return 2;
    }

    uee::VuProgramLog log;
    log.load(argv[1]);
    const std::filesystem::path stem = argv[2];
    const std::filesystem::path declarations = stem.string() + ".decls.h";
    const uee::TranslatedSource source =
        uee::translateVuPrograms(log, units, declarations.filename().string());

    writeFile(declarations, source.declarations);
    for (usize i = 0; i < units; ++i) {
        writeFile(std::format("{}.{}.cpp", stem.string(), i),
                  i < source.units.size() ? source.units[i] : std::string{"namespace uee {}\n"});
    }
    return 0;
}
