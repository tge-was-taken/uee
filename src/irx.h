#pragma once

#include "elf.h"
#include "memory.h"
#include "types.h"

#include <span>
#include <string>
#include <vector>

namespace uee {

// An import the host answers because nothing resident does. The stub becomes a
// syscall carrying this entry's index.
struct Import {
    std::string module;
    u32 id = 0;
};

struct Export {
    std::string module;
    std::vector<u32> entries;
};

struct IrxModule {
    Program program;
    u32 gp = 0;
    std::vector<Export> exports;
};

constexpr u32 importSyscall(u32 index) {
    return (index << 6) | 0x0Cu;
}

constexpr u32 importIndex(u32 word) {
    return (word >> 6) & 0xFFFFFu;
}

// Imports resolve against `resident` first, as the IOP's own linker does. The
// rest are appended to `hostImports`, which modules loaded later share.
struct RomdirEntry {
    std::string name;
    u32 offset = 0;
    u32 size = 0;
};

// A ROMDIR archive: 16-byte entries from RESET to an empty name, then the data,
// each member padded to 16 bytes.
std::vector<RomdirEntry> listRomdir(std::span<const u8> image);

bool isIrx(std::span<const u8> image);

// The name the module gives itself, or an empty string when it gives none.
std::string readIrxName(std::span<const u8> image);

IrxModule loadIrx(std::span<const u8> image, Memory& memory, u32 base,
                  const std::vector<IrxModule>& resident, std::vector<Import>& hostImports);

}  // namespace uee
