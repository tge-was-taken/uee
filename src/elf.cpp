#include "elf.h"

#include <algorithm>
#include <format>
#include <stdexcept>

namespace uee {
namespace {

struct Range {
    u32 start = 0;
    u32 end = 0;

    void add(u32 begin, u32 size) {
        if (size == 0) {
            return;
        }
        if (start == end) {
            start = begin;
            end = begin + size;
            return;
        }
        start = std::min(start, begin);
        end = std::max(end, begin + size);
    }
};

void requireHeader(const ElfImage& image) {
    requireElf32(image);
    const u16 type = image.getHeader().e_type;
    if (type != ET_EXEC) {
        throw std::runtime_error(std::format("ELF type {} is not an executable", type));
    }
}

void copySegment(const ElfImage& image, Memory& memory, const Elf32_Phdr& segment) {
    if (segment.p_offset + segment.p_filesz > image.size()) {
        throw std::runtime_error(std::format(
            "segment at {:08x} reads {} bytes from offset {}, past the end of a {} byte ELF",
            segment.p_vaddr, segment.p_filesz, segment.p_offset, image.size()));
    }
    if (segment.p_memsz < segment.p_filesz) {
        throw std::runtime_error(std::format("segment at {:08x} occupies {} bytes but carries {}",
                                             segment.p_vaddr, segment.p_memsz, segment.p_filesz));
    }

    for (u32 i = 0; i < segment.p_filesz; ++i) {
        memory.write8(segment.p_vaddr + i, image.bytes()[segment.p_offset + i]);
    }
    for (u32 i = segment.p_filesz; i < segment.p_memsz; ++i) {
        memory.write8(segment.p_vaddr + i, 0);
    }
}

Range getCodeSection(const ElfImage& image) {
    const Elf32_Ehdr header = image.getHeader();
    Range code;
    for (u16 i = 0; i < header.e_shnum && header.e_shoff != 0; ++i) {
        const Elf32_Shdr section = image.getSection(header, i);
        if ((section.sh_flags & SHF_EXECINSTR) != 0) {
            code.add(section.sh_addr, section.sh_size);
        }
    }
    return code;
}

std::vector<u32> getFunctionSymbols(const ElfImage& image, Range code) {
    const Elf32_Ehdr header = image.getHeader();
    std::vector<u32> functions;
    for (u16 i = 0; i < header.e_shnum && header.e_shoff != 0; ++i) {
        const Elf32_Shdr section = image.getSection(header, i);
        if (section.sh_type != SHT_SYMTAB) {
            continue;
        }
        for (u32 at = 0; at + sizeof(Elf32_Sym) <= section.sh_size; at += sizeof(Elf32_Sym)) {
            const Elf32_Sym symbol = image.read<Elf32_Sym>(section.sh_offset + at);
            if (ELF32_ST_TYPE(symbol.st_info) != STT_FUNC) {
                continue;
            }
            if (symbol.st_value >= code.start && symbol.st_value < code.end &&
                (symbol.st_value & 3) == 0) {
                functions.push_back(symbol.st_value);
            }
        }
    }
    std::sort(functions.begin(), functions.end());
    functions.erase(std::unique(functions.begin(), functions.end()), functions.end());
    return functions;
}

}  // namespace

Program loadElf(std::span<const u8> bytes, Memory& memory) {
    const ElfImage image{bytes, "ELF"};
    requireHeader(image);
    const Elf32_Ehdr header = image.getHeader();

    Range loaded;
    Range executable;
    for (u16 i = 0; i < header.e_phnum; ++i) {
        const Elf32_Phdr segment = image.getSegment(header, i);
        if (segment.p_type != PT_LOAD) {
            continue;
        }
        copySegment(image, memory, segment);
        loaded.add(segment.p_vaddr, segment.p_memsz);
        if ((segment.p_flags & PF_X) != 0) {
            executable.add(segment.p_vaddr, segment.p_memsz);
        }
    }

    // Segments mix code with data; only sections separate them.
    const Range code = getCodeSection(image);
    const Range span = code.start != code.end ? code : executable;
    return Program{header.e_entry, span.start, span.end,
                   loaded.start, loaded.end, getFunctionSymbols(image, span)};
}

}  // namespace uee
