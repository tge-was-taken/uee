#include "disc.h"

#include <cctype>
#include <cstring>

namespace uee {
namespace {

// ISO 9660.
constexpr u32 kVolumeDescriptorSector = 16;

// Larger than a disc, so a request past this is malformed rather than big.
constexpr u32 kSectorLimit = 0x400000;
constexpr u32 kRootRecordOffset = 156;

constexpr u32 kRecordLength = 0;
constexpr u32 kRecordSector = 2;
constexpr u32 kRecordSize = 10;
constexpr u32 kRecordNameLength = 32;
constexpr u32 kRecordName = 33;

u32 readLittle32(const u8* p) {
    u32 value = 0;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

// The guest asks with a device prefix and version suffix the disc does not have.
std::vector<std::string> splitPath(const std::string& path) {
    const usize colon = path.find(':');
    std::string rest = colon == std::string::npos ? path : path.substr(colon + 1);
    std::vector<std::string> parts;
    std::string part;
    for (const char letter : rest) {
        if (letter == '\\' || letter == '/') {
            if (!part.empty()) {
                parts.push_back(part);
                part.clear();
            }
            continue;
        }
        part.push_back(letter);
    }
    if (!part.empty()) {
        parts.push_back(part);
    }
    return parts;
}

bool namesMatch(const std::string& wanted, const u8* name, u32 length) {
    std::string found(reinterpret_cast<const char*>(name), length);
    const usize version = found.find(';');
    if (version != std::string::npos) {
        found.resize(version);
    }
    std::string target = wanted;
    const usize wantedVersion = target.find(';');
    if (wantedVersion != std::string::npos) {
        target.resize(wantedVersion);
    }
    if (found.size() != target.size()) {
        return false;
    }
    for (usize i = 0; i < found.size(); ++i) {
        const char a = static_cast<char>(std::toupper(static_cast<unsigned char>(found[i])));
        const char b = static_cast<char>(std::toupper(static_cast<unsigned char>(target[i])));
        if (a != b) {
            return false;
        }
    }
    return true;
}

}  // namespace

Disc::Disc(const std::string& path) : mFile(std::fopen(path.c_str(), "rb")) {}

Disc::~Disc() {
    if (mFile != nullptr) {
        std::fclose(mFile);
    }
}

bool Disc::read(u32 sector, u32 count, std::vector<u8>& out) {
    if (mFile == nullptr || count > kSectorLimit) {
        return false;
    }
    out.assign(static_cast<usize>(count) * kSectorBytes, 0);
    if (std::fseek(mFile, static_cast<long>(sector) * kSectorBytes, SEEK_SET) != 0) {
        return false;
    }
    return std::fread(out.data(), 1, out.size(), mFile) == out.size();
}

std::optional<DiscFile> Disc::find(const std::string& path) {
    std::vector<u8> sector;
    if (!read(kVolumeDescriptorSector, 1, sector)) {
        return std::nullopt;
    }
    const u8* root = sector.data() + kRootRecordOffset;
    DiscFile current{readLittle32(root + kRecordSector), readLittle32(root + kRecordSize), ""};

    for (const std::string& part : splitPath(path)) {
        const std::optional<DiscFile> next = findInDirectory(current, part);
        if (!next) {
            return std::nullopt;
        }
        current = *next;
    }
    return current;
}

std::optional<DiscFile> Disc::findInDirectory(const DiscFile& directory,
                                              const std::string& name) {
    const u32 sectors = (directory.size + kSectorBytes - 1) / kSectorBytes;
    std::vector<u8> data;
    if (sectors == 0 || !read(directory.sector, sectors, data)) {
        return std::nullopt;
    }

    usize offset = 0;
    while (offset + kRecordName < data.size()) {
        const u8 length = data[offset + kRecordLength];
        if (length == 0) {
            // Records never straddle a sector, so this one is padding to its end.
            offset = (offset / kSectorBytes + 1) * kSectorBytes;
            continue;
        }
        const u8 nameLength = data[offset + kRecordNameLength];
        if (namesMatch(name, data.data() + offset + kRecordName, nameLength)) {
            const char* found = reinterpret_cast<const char*>(data.data() + offset + kRecordName);
            return DiscFile{readLittle32(data.data() + offset + kRecordSector),
                            readLittle32(data.data() + offset + kRecordSize),
                            std::string(found, nameLength)};
        }
        offset += length;
    }
    return std::nullopt;
}

}  // namespace uee
