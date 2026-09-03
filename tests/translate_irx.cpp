#include "log.h"
#include "iop_run.h"
#include "iop_translate.h"
#include "disc.h"
#include "iop.h"
#include "patches.h"
#include "irx.h"
#include "memory.h"
#include "types.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <span>
#include <optional>
#include <cstdio>
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

// Every module image on a disc: the loose IRX files, and the members of the
// IOPRP archive, which is where the kernel modules live.
struct DiscModule {
    std::string name;
    std::vector<u8> image;
    u32 base = 0;
    u32 end = 0;
};

void walkDisc(uee::Disc& disc, const uee::DiscFile& directory,
              std::vector<uee::DiscFile>& files) {
    for (const uee::DiscFile& entry : disc.list(directory)) {
        if (entry.isDirectory) {
            walkDisc(disc, entry, files);
        } else {
            files.push_back(entry);
        }
    }
}

// ISO9660 names carry a ";1" version suffix the guest never uses.
std::string withoutVersion(const std::string& name) {
    const usize semicolon = name.find(';');
    return semicolon == std::string::npos ? name : name.substr(0, semicolon);
}

std::vector<DiscModule> findModules(uee::Disc& disc, const std::vector<std::string>& hosted) {
    const std::optional<uee::DiscFile> root = disc.getRoot();
    if (!root) {
        spdlog::critical("the disc has no root directory");
        std::exit(2);
    }
    std::vector<uee::DiscFile> files;
    walkDisc(disc, *root, files);

    // The host list holds bare lowercase names, the way the loader compares them.
    const auto isHosted = [&](const std::string& name) {
        std::string bare = withoutVersion(name);
        bare = bare.substr(0, bare.find('.'));
        for (char& letter : bare) {
            letter = static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
        }
        return std::find(hosted.begin(), hosted.end(), bare) != hosted.end();
    };
    const auto readWhole = [&](const uee::DiscFile& file) {
        std::vector<u8> data;
        disc.read(file.sector, (file.size + 2047) / 2048, data);
        data.resize(file.size);
        return data;
    };

    std::vector<DiscModule> modules;
    for (const uee::DiscFile& file : files) {
        const std::string name = withoutVersion(file.name);
        if (name.ends_with(".IRX") && !isHosted(name)) {
            modules.push_back(DiscModule{name, readWhole(file)});
        } else if (name.ends_with(".IMG")) {
            const std::vector<u8> archive = readWhole(file);
            for (const uee::RomdirEntry& member : uee::listRomdir(archive)) {
                if (member.size == 0 || isHosted(member.name) ||
                    member.offset + member.size > archive.size()) {
                    continue;
                }
                modules.push_back(DiscModule{member.name,
                                             {archive.begin() + member.offset,
                                              archive.begin() + member.offset + member.size}});
            }
        }
    }
    // Sorted so the packing below is the same on every run and every machine.
    std::sort(modules.begin(), modules.end(),
              [](const DiscModule& a, const DiscModule& b) { return a.name < b.name; });
    return modules;
}

// Each module owns a fixed slot, so the address it is translated for is the one
// the loader puts it at, whatever order the game loads them in.
void packBases(std::vector<DiscModule>& modules) {
    u32 next = uee::kIopModuleBase;
    for (DiscModule& module : modules) {
        uee::Memory memory = uee::makeIopMemory();
        std::vector<uee::Import> imports;
        try {
            const uee::IrxModule loaded = uee::loadIrx(module.image, memory, next, {}, imports);
            module.base = next;
            module.end = loaded.program.imageEnd;
            next = (module.end + 0xF) & ~0xFu;
        } catch (const std::exception& error) {
            // The manifest is the only thing on stdout, so a build can read it.
            std::fprintf(stderr, "%s is not a module this can load: %s\n", module.name.c_str(),
                         error.what());
        }
    }
    if (next > uee::kIopHeapBase) {
        spdlog::critical("the modules need {:#x}..{:#x}, past the {:#x} the heap starts at",
                         uee::kIopModuleBase, next, uee::kIopHeapBase);
        std::exit(2);
    }
}

// The generated code is named after the module, so the name has to be one an
// identifier can be built from.
std::string symbolNameOf(const std::string& name) {
    std::string symbol = name.substr(0, name.find('.'));
    for (char& letter : symbol) {
        if (!std::isalnum(static_cast<unsigned char>(letter))) {
            letter = '_';
        }
    }
    return symbol;
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

int translateProgram(uee::Memory& memory, const uee::IrxModule& module,
                     const std::vector<uee::Import>& imports, const std::filesystem::path& stem,
                     const std::string& name, usize units, const std::string& loadName,
                     const std::set<std::string>& guestLibraries) {
    // Nothing in the image reaches an exported entry, so discovery is told.
    std::vector<u32> entries;
    for (const uee::Export& exported : module.exports) {
        entries.insert(entries.end(), exported.entries.begin(), exported.entries.end());
    }

    const std::filesystem::path declarations = stem.string() + ".decls.h";
    const uee::TranslatedSource source =
        uee::translateIop(memory, module.program, name, imports, units,
                          declarations.filename().string(), loadName, entries, guestLibraries);

    writeFile(declarations, source.declarations);
    for (usize i = 0; i < units; ++i) {
        writeFile(std::format("{}.{}.cpp", stem.string(), i),
                  i < source.units.size() ? source.units[i]
                                          : std::string{"namespace uee {}\n"});
    }
    return 0;
}

int translateOne(std::span<const u8> image, const std::filesystem::path& stem,
                 const std::string& name, usize units, u32 base, const std::string& loadName) {
    uee::Memory memory = uee::makeIopMemory();
    std::vector<uee::Import> imports;
    const uee::IrxModule module = uee::loadIrx(image, memory, base, {}, imports);
    return translateProgram(memory, module, imports, stem, name, units, loadName, {});
}

// Names the modules and the slot each one owns, for a build system to turn into
// one translation per module.
int listDiscModules(const std::string& iso, const std::string& config) {
    uee::Disc disc{iso};
    if (!disc.isOpen()) {
        spdlog::critical("cannot open disc {}", iso);
        return 2;
    }
    std::vector<DiscModule> modules =
        findModules(disc, config.empty() ? std::vector<std::string>{}
                                         : uee::loadGameConfig(config).hostModules);
    packBases(modules);
    for (const DiscModule& module : modules) {
        if (module.end != 0) {
            std::printf("%s %#010x %#010x\n", module.name.c_str(), module.base, module.end);
        }
    }
    return 0;
}

int translateDiscModule(const std::string& iso, const std::string& config,
                        const std::string& wanted, const std::filesystem::path& stem,
                        usize units) {
    uee::Disc disc{iso};
    if (!disc.isOpen()) {
        spdlog::critical("cannot open disc {}", iso);
        return 2;
    }
    std::vector<DiscModule> modules =
        findModules(disc, config.empty() ? std::vector<std::string>{}
                                         : uee::loadGameConfig(config).hostModules);
    packBases(modules);
    // Every module's exports first, because the order they are packed in is not
    // the order the game loads them, and an import binds to whichever module
    // answers it whenever that module arrived.
    uee::Memory scratch = uee::makeIopMemory();
    std::vector<uee::IrxModule> resident;
    std::set<std::string> guestLibraries;
    for (const DiscModule& module : modules) {
        if (module.end == 0) {
            continue;
        }
        std::vector<uee::Import> ignored;
        const uee::IrxModule loaded =
            uee::loadIrx(module.image, scratch, module.base, {}, ignored);
        for (const uee::Export& exported : loaded.exports) {
            guestLibraries.insert(exported.module);
        }
        resident.push_back(loaded);
    }

    for (const DiscModule& module : modules) {
        if (module.name != wanted) {
            continue;
        }
        uee::Memory memory = uee::makeIopMemory();
        std::vector<uee::Import> imports;
        const uee::IrxModule target =
            uee::loadIrx(module.image, memory, module.base, resident, imports);
        return translateProgram(memory, target, imports, stem, symbolNameOf(wanted), units,
                                wanted, guestLibraries);
    }
    spdlog::critical("the disc has no module named {}", wanted);
    return 2;
}

int main(int argc, char** argv) {
    uee::configureLogging();
    if (argc >= 3 && std::string_view(argv[1]) == "--list") {
        return listDiscModules(argv[2], argc > 3 ? argv[3] : "");
    }
    if (argc >= 7 && std::string_view(argv[1]) == "--module") {
        return translateDiscModule(argv[3], argv[4], argv[2], argv[5], parseCount(argv[6]));
    }
    if (argc < 5) {
        spdlog::error("usage: {} <test.irx> <out-stem> <name> <units> [base] [load-name]",
                      argv[0]);
        return 2;
    }

    const std::filesystem::path stem = argv[2];
    const usize units = parseCount(argv[4]);
    const u32 base = argc > 5 ? static_cast<u32>(std::strtoul(argv[5], nullptr, 0))
                              : uee::kIopModuleBase;

    return translateOne(readFile(argv[1]), stem, argv[3], units, base,
                        argc > 6 ? argv[6] : "");
}
