#include "irx.h"

#include <cstring>
#include <format>
#include <optional>
#include <stdexcept>

namespace uee {
namespace {

constexpr u16 kIrxType = 0xFF80;
constexpr u32 kIopModSection = 0x70000080;

constexpr u32 kImportMagic = 0x41E00000;
constexpr u32 kExportMagic = 0x41C00000;
constexpr u32 kExportEntryOffset = 20;
// The first entries are the module's own hooks, not library functions.
constexpr u32 kExportReserved = 4;
constexpr u32 kImportNameOffset = 12;
constexpr u32 kImportNameSize = 8;
constexpr u32 kImportEntryOffset = 20;
constexpr u32 kImportEntrySize = 8;

void requireHeader(const ElfImage& image) {
    requireElf32(image);
    const u16 type = image.getHeader().e_type;
    if (type != kIrxType) {
        throw std::runtime_error(std::format("ELF type {:04x} is not an IRX module", type));
    }
}

std::vector<Elf32_Shdr> readSections(const ElfImage& image) {
    const Elf32_Ehdr header = image.getHeader();
    std::vector<Elf32_Shdr> sections;
    for (u16 i = 0; i < header.e_shnum; ++i) {
        sections.push_back(image.getSection(header, i));
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

Elf32_Shdr findModuleSection(const std::vector<Elf32_Shdr>& sections) {
    for (const Elf32_Shdr& section : sections) {
        if (section.sh_type == kIopModSection) {
            return section;
        }
    }
    throw std::runtime_error("IRX carries no .iopmod section");
}

ModuleHeader readModuleHeader(const ElfImage& image, const std::vector<Elf32_Shdr>& sections) {
    const u32 at = findModuleSection(sections).sh_offset;
    return {image.read<u32>(at + 4), image.read<u32>(at + 8), image.read<u32>(at + 12),
            image.read<u32>(at + 16), image.read<u32>(at + 20)};
}

void copySegments(const ElfImage& image, Memory& memory, u32 base) {
    const Elf32_Ehdr header = image.getHeader();
    for (u16 i = 0; i < header.e_phnum; ++i) {
        const Elf32_Phdr segment = image.getSegment(header, i);
        if (segment.p_type != PT_LOAD) {
            continue;
        }
        const u32 address = base + segment.p_vaddr;
        if (segment.p_offset + segment.p_filesz > image.size()) {
            throw std::runtime_error(std::format(
                "segment at {:08x} reads {} bytes from offset {}, past the end of a {} byte IRX",
                address, segment.p_filesz, segment.p_offset, image.size()));
        }
        for (u32 j = 0; j < segment.p_filesz; ++j) {
            memory.write8(address + j, image.bytes()[segment.p_offset + j]);
        }
        for (u32 j = segment.p_filesz; j < segment.p_memsz; ++j) {
            memory.write8(address + j, 0);
        }
    }
}

// The addend already holds the address the linker gave the symbol.
void relocate(Memory& memory, u32 base, const std::vector<Elf32_Shdr>& sections,
              const ElfImage& image) {
    for (const Elf32_Shdr& section : sections) {
        if (section.sh_type != SHT_REL) {
            continue;
        }
        // Debug relocations carry offsets that are only meaningful inside
        // their own section.
        if (section.sh_info >= sections.size() ||
            (sections[section.sh_info].sh_flags & SHF_ALLOC) == 0) {
            continue;
        }
        constexpr u32 kEntry = sizeof(Elf32_Rel);
        for (u32 i = 0; i + kEntry <= section.sh_size; i += kEntry) {
            const Elf32_Rel entry = image.read<Elf32_Rel>(section.sh_offset + i);
            const u32 where = base + entry.r_offset;
            const u32 word = memory.read32(where);
            switch (ELF32_R_TYPE(entry.r_info)) {
                case R_MIPS_32:
                    memory.write32(where, word + base);
                    break;
                case R_MIPS_26: {
                    const u32 target = ((word & 0x03FFFFFFu) << 2) + base;
                    memory.write32(where, (word & ~0x03FFFFFFu) | ((target >> 2) & 0x03FFFFFFu));
                    break;
                }
                case R_MIPS_HI16: {
                    // gas emits the pair adjacent; a shared high half is not handled.
                    const Elf32_Rel next =
                        i + 2 * kEntry <= section.sh_size
                            ? image.read<Elf32_Rel>(section.sh_offset + i + kEntry)
                            : Elf32_Rel{};
                    if (i + 2 * kEntry > section.sh_size ||
                        ELF32_R_TYPE(next.r_info) != R_MIPS_LO16) {
                        throw std::runtime_error(std::format(
                            "the high half relocation at {:08x} is not followed by its low half",
                            where));
                    }
                    const u32 lowWhere = base + next.r_offset;
                    const u32 lowWord = memory.read32(lowWhere);
                    const u32 value = ((word & 0xFFFFu) << 16) +
                                      static_cast<u32>(static_cast<s16>(lowWord & 0xFFFFu)) + base;
                    const u32 upper = (value >> 16) + ((value & 0x8000u) != 0 ? 1u : 0u);
                    memory.write32(where, (word & 0xFFFF0000u) | (upper & 0xFFFFu));
                    memory.write32(lowWhere, (lowWord & 0xFFFF0000u) | (value & 0xFFFFu));
                    i += kEntry;
                    break;
                }
                case R_MIPS_LO16:
                    throw std::runtime_error(
                        std::format("the low half relocation at {:08x} has no high half", where));
                default:
                    throw std::runtime_error(std::format("relocation type {} at {:08x} is not one "
                                                         "an IRX module is built with",
                                                         ELF32_R_TYPE(entry.r_info), where));
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

std::vector<RomdirEntry> listRomdir(std::span<const u8> image) {
    constexpr usize kEntryBytes = 16;
    constexpr usize kNameBytes = 10;
    constexpr usize kExtInfoSize = 10;
    constexpr usize kSize = 12;
    if (image.size() < kEntryBytes || std::memcmp(image.data(), "RESET", 5) != 0) {
        return {};
    }

    std::vector<RomdirEntry> entries;
    u32 offset = 0;
    for (usize at = 0; at + kEntryBytes <= image.size(); at += kEntryBytes) {
        const char* name = reinterpret_cast<const char*>(image.data() + at);
        if (name[0] == '\0') {
            break;
        }
        u32 size = 0;
        std::memcpy(&size, image.data() + at + kSize, sizeof(size));
        entries.push_back(RomdirEntry{std::string(name, ::strnlen(name, kNameBytes)), offset, size});
        offset += (size + 15) & ~15u;
        (void)kExtInfoSize;
    }

    // The directory itself is the first member, so every offset is past it.
    const u32 dataStart = entries.empty() ? 0 : entries.front().size;
    for (RomdirEntry& entry : entries) {
        entry.offset += (dataStart + 15) & ~15u;
    }
    return entries;
}

namespace {

std::string readString(const ElfImage& image, usize offset, usize end) {
    std::string text;
    for (usize at = offset; at < end; ++at) {
        const char letter = image.read<char>(at);
        if (letter == '\0') {
            break;
        }
        text.push_back(letter);
    }
    return text;
}

// Where a module address lies in the file, before it is relocated.
std::optional<usize> findFileOffset(const ElfImage& image, u32 address) {
    const Elf32_Ehdr header = image.getHeader();
    for (u16 i = 0; i < header.e_phnum; ++i) {
        const Elf32_Phdr segment = image.getSegment(header, i);
        if (segment.p_type == PT_LOAD && address >= segment.p_vaddr &&
            address < segment.p_vaddr + segment.p_filesz) {
            return segment.p_offset + (address - segment.p_vaddr);
        }
    }
    return std::nullopt;
}

}  // namespace

bool isIrx(std::span<const u8> bytes) {
    constexpr usize kTypeOffset = 16;
    return bytes.size() > kTypeOffset + 2 && bytes[0] == 0x7F && bytes[1] == 'E' &&
           bytes[2] == 'L' && bytes[3] == 'F' &&
           (bytes[kTypeOffset] | (bytes[kTypeOffset + 1] << 8)) == kIrxType;
}

// A module built with IRX_ID leaves the header's name empty and points at a
// {name, version} pair in its own data instead.
std::string readIrxName(std::span<const u8> bytes) {
    constexpr u32 kNameOffset = 0x1A;
    constexpr u32 kNoModuleInfo = 0xFFFFFFFF;
    const ElfImage image{bytes, "IRX"};
    requireHeader(image);
    const Elf32_Shdr section = findModuleSection(readSections(image));
    const std::string name = readString(image, section.sh_offset + kNameOffset,
                                        section.sh_offset + section.sh_size);
    const u32 moduleInfo = image.read<u32>(section.sh_offset);
    if (!name.empty() || moduleInfo == kNoModuleInfo) {
        return name;
    }
    const std::optional<usize> info = findFileOffset(image, moduleInfo);
    if (!info) {
        return name;
    }
    const std::optional<usize> text = findFileOffset(image, image.read<u32>(*info));
    return text ? readString(image, *text, bytes.size()) : name;
}

IrxModule loadIrx(std::span<const u8> bytes, Memory& memory, u32 base,
                  const std::vector<IrxModule>& resident, std::vector<Import>& hostImports) {
    const ElfImage image{bytes, "IRX"};
    requireHeader(image);
    const std::vector<Elf32_Shdr> sections = readSections(image);
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
