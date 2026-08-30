#include "irx.h"

#include <format>
#include <optional>
#include <stdexcept>

namespace uee {
namespace {

constexpr u16 kIrxType = 0xFF80;
constexpr u16 kMachineMips = 8;
constexpr u32 kIopModSection = 0x70000080;
constexpr u32 kRelocationSection = 9;
constexpr u32 kLoadableSegment = 1;

constexpr u32 kRelocate32 = 2;
constexpr u32 kRelocateJump = 4;
constexpr u32 kRelocateUpper = 5;
constexpr u32 kRelocateLower = 6;

constexpr u32 kImportMagic = 0x41E00000;
constexpr u32 kExportMagic = 0x41C00000;
constexpr u32 kExportEntryOffset = 20;
// The first entries are the module's own hooks, not library functions.
constexpr u32 kExportReserved = 4;
constexpr u32 kImportNameOffset = 12;
constexpr u32 kImportNameSize = 8;
constexpr u32 kImportEntryOffset = 20;
constexpr u32 kImportEntrySize = 8;

struct Section {
    u32 type = 0;
    u32 flags = 0;
    u32 address = 0;
    u32 offset = 0;
    u32 size = 0;
    u32 target = 0;
};

constexpr u32 kAllocated = 2;

u32 readU32(std::span<const u8> image, usize offset) {
    if (offset + 4 > image.size()) {
        throw std::runtime_error(std::format("IRX ends before offset {}", offset));
    }
    return static_cast<u32>(image[offset]) | (static_cast<u32>(image[offset + 1]) << 8) |
           (static_cast<u32>(image[offset + 2]) << 16) |
           (static_cast<u32>(image[offset + 3]) << 24);
}

u16 readU16(std::span<const u8> image, usize offset) {
    if (offset + 2 > image.size()) {
        throw std::runtime_error(std::format("IRX ends before offset {}", offset));
    }
    return static_cast<u16>(image[offset] | (image[offset + 1] << 8));
}

void requireHeader(std::span<const u8> image) {
    if (image.size() < 52) {
        throw std::runtime_error(
            std::format("IRX is {} bytes, too short for a header", image.size()));
    }
    if (image[0] != 0x7F || image[1] != 'E' || image[2] != 'L' || image[3] != 'F') {
        throw std::runtime_error("IRX magic missing");
    }
    if (image[4] != 1 || image[5] != 1) {
        throw std::runtime_error("IRX is not a 32-bit little-endian ELF");
    }
    const u16 type = readU16(image, 16);
    if (type != kIrxType) {
        throw std::runtime_error(std::format("ELF type {:04x} is not an IRX module", type));
    }
    const u16 machine = readU16(image, 18);
    if (machine != kMachineMips) {
        throw std::runtime_error(std::format("IRX machine {} is not MIPS", machine));
    }
}

std::vector<Section> readSections(std::span<const u8> image) {
    const u32 table = readU32(image, 32);
    const u16 entrySize = readU16(image, 46);
    const u16 count = readU16(image, 48);

    std::vector<Section> sections;
    for (u16 i = 0; i < count; ++i) {
        const usize header = table + static_cast<usize>(i) * entrySize;
        sections.push_back({readU32(image, header + 4), readU32(image, header + 8),
                            readU32(image, header + 12), readU32(image, header + 16),
                            readU32(image, header + 20), readU32(image, header + 28)});
    }
    return sections;
}

struct ModuleHeader {
    u32 entry = 0;
    u32 gp = 0;
    u32 textSize = 0;
    u32 dataSize = 0;
    u32 bssSize = 0;
};

ModuleHeader readModuleHeader(std::span<const u8> image, const std::vector<Section>& sections) {
    for (const Section& section : sections) {
        if (section.type != kIopModSection) {
            continue;
        }
        return {readU32(image, section.offset + 4), readU32(image, section.offset + 8),
                readU32(image, section.offset + 12), readU32(image, section.offset + 16),
                readU32(image, section.offset + 20)};
    }
    throw std::runtime_error("IRX carries no .iopmod section");
}

void copySegments(std::span<const u8> image, Memory& memory, u32 base) {
    const u32 table = readU32(image, 28);
    const u16 entrySize = readU16(image, 42);
    const u16 count = readU16(image, 44);

    for (u16 i = 0; i < count; ++i) {
        const usize header = table + static_cast<usize>(i) * entrySize;
        if (readU32(image, header) != kLoadableSegment) {
            continue;
        }
        const u32 fileOffset = readU32(image, header + 4);
        const u32 address = base + readU32(image, header + 8);
        const u32 fileSize = readU32(image, header + 16);
        const u32 memorySize = readU32(image, header + 20);
        if (fileOffset + fileSize > image.size()) {
            throw std::runtime_error(std::format(
                "segment at {:08x} reads {} bytes from offset {}, past the end of a {} byte IRX",
                address, fileSize, fileOffset, image.size()));
        }
        for (u32 j = 0; j < fileSize; ++j) {
            memory.write8(address + j, image[fileOffset + j]);
        }
        for (u32 j = fileSize; j < memorySize; ++j) {
            memory.write8(address + j, 0);
        }
    }
}

// The addend already holds the address the linker gave the symbol.
void relocate(Memory& memory, u32 base, const std::vector<Section>& sections,
              std::span<const u8> image) {
    for (const Section& section : sections) {
        if (section.type != kRelocationSection) {
            continue;
        }
        // Debug relocations carry offsets that are only meaningful inside
        // their own section.
        if (section.target >= sections.size() ||
            (sections[section.target].flags & kAllocated) == 0) {
            continue;
        }
        for (u32 i = 0; i + 8 <= section.size; i += 8) {
            const u32 where = base + readU32(image, section.offset + i);
            const u32 info = readU32(image, section.offset + i + 4);
            const u32 word = memory.read32(where);
            switch (info & 0xFFu) {
                case kRelocate32:
                    memory.write32(where, word + base);
                    break;
                case kRelocateJump: {
                    const u32 target = ((word & 0x03FFFFFFu) << 2) + base;
                    memory.write32(where, (word & ~0x03FFFFFFu) | ((target >> 2) & 0x03FFFFFFu));
                    break;
                }
                case kRelocateUpper: {
                    // gas emits the pair adjacent; a shared high half is not handled.
                    if (i + 16 > section.size ||
                        (readU32(image, section.offset + i + 12) & 0xFFu) != kRelocateLower) {
                        throw std::runtime_error(std::format(
                            "the high half relocation at {:08x} is not followed by its low half",
                            where));
                    }
                    const u32 lowWhere = base + readU32(image, section.offset + i + 8);
                    const u32 lowWord = memory.read32(lowWhere);
                    const u32 value = ((word & 0xFFFFu) << 16) +
                                      static_cast<u32>(static_cast<s16>(lowWord & 0xFFFFu)) + base;
                    const u32 upper = (value >> 16) + ((value & 0x8000u) != 0 ? 1u : 0u);
                    memory.write32(where, (word & 0xFFFF0000u) | (upper & 0xFFFFu));
                    memory.write32(lowWhere, (lowWord & 0xFFFF0000u) | (value & 0xFFFFu));
                    i += 8;
                    break;
                }
                case kRelocateLower:
                    throw std::runtime_error(
                        std::format("the low half relocation at {:08x} has no high half", where));
                default:
                    throw std::runtime_error(std::format("relocation type {} at {:08x} is not one "
                                                         "an IRX module is built with",
                                                         info & 0xFFu, where));
            }
        }
    }
}

std::string readName(Memory& memory, u32 address) {
    std::string name;
    for (u32 i = 0; i < kImportNameSize; ++i) {
        const u8 byte = memory.read8(address + i);
        if (byte == 0) {
            break;
        }
        name.push_back(static_cast<char>(byte));
    }
    return name;
}

std::vector<Export> readExports(Memory& memory, u32 textStart, u32 textEnd) {
    std::vector<Export> exports;
    for (u32 address = textStart; address + 4 <= textEnd; address += 4) {
        if (memory.read32(address) != kExportMagic) {
            continue;
        }
        Export offered{readName(memory, address + kImportNameOffset), {}};
        u32 entry = address + kExportEntryOffset;
        // A library ends at the first empty slot past the hooks, which may
        // themselves be empty.
        for (u32 index = 0; entry + 4 <= textEnd; ++index, entry += 4) {
            const u32 target = memory.read32(entry);
            if (target == 0 && index >= kExportReserved) {
                break;
            }
            offered.entries.push_back(target);
        }
        exports.push_back(std::move(offered));
        address = entry;
    }
    return exports;
}

u32 findExport(const std::vector<IrxModule>& resident, const std::string& module, u32 id) {
    for (const IrxModule& loaded : resident) {
        for (const Export& offered : loaded.exports) {
            if (offered.module == module && id < offered.entries.size()) {
                return offered.entries[id];
            }
        }
    }
    return 0;
}

// The IOP's linker turns a stub's `jr $ra` into a jump to the resident
// function, leaving the delay slot holding the function number alone.
void linkImports(Memory& memory, u32 textStart, u32 textEnd,
                 const std::vector<IrxModule>& resident, std::vector<Import>& hostImports) {
    for (u32 address = textStart; address + 4 <= textEnd; address += 4) {
        if (memory.read32(address) != kImportMagic) {
            continue;
        }
        const std::string module = readName(memory, address + kImportNameOffset);
        u32 entry = address + kImportEntryOffset;
        while (entry + kImportEntrySize <= textEnd &&
               (memory.read32(entry) != 0 || memory.read32(entry + 4) != 0)) {
            const u32 id = memory.read32(entry + 4) & 0xFFFFu;
            const u32 resolved = findExport(resident, module, id);
            if (resolved != 0) {
                memory.write32(entry, 0x08000000u | ((resolved >> 2) & 0x03FFFFFFu));
            } else {
                hostImports.push_back({module, id});
                memory.write32(entry, importSyscall(static_cast<u32>(hostImports.size() - 1)));
            }
            entry += kImportEntrySize;
        }
        address = entry;
    }
}

}  // namespace

IrxModule loadIrx(std::span<const u8> image, Memory& memory, u32 base,
                  const std::vector<IrxModule>& resident, std::vector<Import>& hostImports) {
    requireHeader(image);
    const std::vector<Section> sections = readSections(image);
    const ModuleHeader header = readModuleHeader(image, sections);

    copySegments(image, memory, base);
    relocate(memory, base, sections, image);
    // A write outside memory is dropped, so without this the module would run
    // with whatever was already there.
    if (const std::optional<u32> address = memory.takeFault()) {
        throw std::runtime_error(std::format(
            "the module reaches {:08x}, which is not mapped, so it cannot be loaded at {:08x}",
            *address, base));
    }

    const u32 textEnd = base + header.textSize;
    const u32 imageEnd = base + header.textSize + header.dataSize + header.bssSize;
    linkImports(memory, base, textEnd, resident, hostImports);
    return IrxModule{Program{base + header.entry, base, textEnd, base, imageEnd},
                     base + header.gp, readExports(memory, base, textEnd)};
}

}  // namespace uee
