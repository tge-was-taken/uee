#pragma once

#include "memory.h"
#include "types.h"

#include <cstring>
#include <format>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace uee {

struct Elf32_Ehdr {
    u8 e_ident[16];
    u16 e_type;
    u16 e_machine;
    u32 e_version;
    u32 e_entry;
    u32 e_phoff;
    u32 e_shoff;
    u32 e_flags;
    u16 e_ehsize;
    u16 e_phentsize;
    u16 e_phnum;
    u16 e_shentsize;
    u16 e_shnum;
    u16 e_shstrndx;
};

struct Elf32_Phdr {
    u32 p_type;
    u32 p_offset;
    u32 p_vaddr;
    u32 p_paddr;
    u32 p_filesz;
    u32 p_memsz;
    u32 p_flags;
    u32 p_align;
};

struct Elf32_Shdr {
    u32 sh_name;
    u32 sh_type;
    u32 sh_flags;
    u32 sh_addr;
    u32 sh_offset;
    u32 sh_size;
    u32 sh_link;
    u32 sh_info;
    u32 sh_addralign;
    u32 sh_entsize;
};

struct Elf32_Sym {
    u32 st_name;
    u32 st_value;
    u32 st_size;
    u8 st_info;
    u8 st_other;
    u16 st_shndx;
};

struct Elf32_Rel {
    u32 r_offset;
    u32 r_info;
};

static_assert(sizeof(Elf32_Ehdr) == 52);
static_assert(sizeof(Elf32_Phdr) == 32);
static_assert(sizeof(Elf32_Shdr) == 40);
static_assert(sizeof(Elf32_Sym) == 16);
static_assert(sizeof(Elf32_Rel) == 8);

enum : usize {
    EI_MAG0 = 0,
    EI_MAG1 = 1,
    EI_MAG2 = 2,
    EI_MAG3 = 3,
    EI_CLASS = 4,
    EI_DATA = 5,
};

enum : u8 { ELFCLASS32 = 1, ELFDATA2LSB = 1 };
enum : u16 { ET_EXEC = 2, EM_MIPS = 8 };
enum : u32 { PT_LOAD = 1, PF_X = 1 };
enum : u32 { SHF_ALLOC = 2, SHF_EXECINSTR = 4 };
enum : u32 { SHT_SYMTAB = 2, SHT_REL = 9 };
enum : u8 { STT_FUNC = 2 };
enum : u32 { R_MIPS_32 = 2, R_MIPS_26 = 4, R_MIPS_HI16 = 5, R_MIPS_LO16 = 6 };

constexpr u32 ELF32_R_TYPE(u32 info) {
    return info & 0xFFu;
}

constexpr u8 ELF32_ST_TYPE(u8 info) {
    return static_cast<u8>(info & 0xFu);
}

// A header table is indexed by the size the file declares, not by sizeof, so a
// producer that pads its entries still reads correctly.
class ElfImage {
public:
    ElfImage(std::span<const u8> bytes, std::string_view format)
        : mBytes(bytes), mFormat(format) {}

    template <typename T>
    T read(usize offset) const {
        if (offset + sizeof(T) > mBytes.size()) {
            throw std::runtime_error(std::format("{} ends before offset {}", mFormat, offset));
        }
        T value;
        std::memcpy(&value, mBytes.data() + offset, sizeof(T));
        return value;
    }

    Elf32_Ehdr getHeader() const { return read<Elf32_Ehdr>(0); }

    Elf32_Phdr getSegment(const Elf32_Ehdr& header, u16 index) const {
        return read<Elf32_Phdr>(header.e_phoff +
                                static_cast<usize>(index) * header.e_phentsize);
    }

    Elf32_Shdr getSection(const Elf32_Ehdr& header, u16 index) const {
        return read<Elf32_Shdr>(header.e_shoff +
                                static_cast<usize>(index) * header.e_shentsize);
    }

    std::span<const u8> bytes() const { return mBytes; }
    usize size() const { return mBytes.size(); }
    std::string_view format() const { return mFormat; }

private:
    std::span<const u8> mBytes;
    std::string_view mFormat;
};

inline void requireElf32(const ElfImage& image) {
    if (image.size() < sizeof(Elf32_Ehdr)) {
        throw std::runtime_error(std::format("{} is {} bytes, too short for a header",
                                             image.format(), image.size()));
    }
    const Elf32_Ehdr header = image.getHeader();
    if (header.e_ident[EI_MAG0] != 0x7F || header.e_ident[EI_MAG1] != 'E' ||
        header.e_ident[EI_MAG2] != 'L' || header.e_ident[EI_MAG3] != 'F') {
        throw std::runtime_error(std::format("{} magic missing", image.format()));
    }
    if (header.e_ident[EI_CLASS] != ELFCLASS32) {
        throw std::runtime_error(std::format("{} class {} is not 32-bit", image.format(),
                                             header.e_ident[EI_CLASS]));
    }
    if (header.e_ident[EI_DATA] != ELFDATA2LSB) {
        throw std::runtime_error(std::format("{} data encoding {} is not little-endian",
                                             image.format(), header.e_ident[EI_DATA]));
    }
    if (header.e_machine != EM_MIPS) {
        throw std::runtime_error(
            std::format("{} machine {} is not MIPS", image.format(), header.e_machine));
    }
}

struct Program {
    u32 entry = 0;
    u32 codeStart = 0;
    u32 codeEnd = 0;
    u32 imageStart = 0;
    u32 imageEnd = 0;
    // Where the symbol table says a function starts. A virtual call reaches its
    // targets through a table, so nothing in the code points at them.
    std::vector<u32> functions;
};

Program loadElf(std::span<const u8> image, Memory& memory);

}  // namespace uee
