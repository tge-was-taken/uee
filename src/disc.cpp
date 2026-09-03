#include "disc.h"

#include <cctype>
#include <cstring>
#include <filesystem>

namespace uee {
namespace {

// ECMA-119: the primary volume descriptor, and its root record at 156.
constexpr u32 kVolumeDescriptorSector = 16;
constexpr u32 kRootRecordOffset = 156;

// Larger than a disc, so a request past this is malformed rather than big.
constexpr u32 kSectorLimit = 0x400000;

// ECMA-119 directory record. The name follows the fixed part, so the two
// big-endian copies are read and ignored rather than skipped over.
#pragma pack(push, 1)
struct DirectoryRecord {
    u8 length;
    u8 extendedAttributeLength;
    u32 extent;
    u32 extentBigEndian;
    u32 dataLength;
    u32 dataLengthBigEndian;
    u8 recordedAt[7];
    u8 fileFlags;
    u8 fileUnitSize;
    u8 interleaveGap;
    u16 volumeSequence;
    u16 volumeSequenceBigEndian;
    u8 nameLength;
};
#pragma pack(pop)
static_assert(sizeof(DirectoryRecord) == 33);

constexpr u8 kDirectoryFlag = 0x02;

DirectoryRecord readRecord(const u8* bytes) {
    DirectoryRecord record;
    std::memcpy(&record, bytes, sizeof(record));
    return record;
}

const char* nameOf(const u8* bytes) {
    return reinterpret_cast<const char*>(bytes + sizeof(DirectoryRecord));
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

// Reads fail after the open succeeds, which looks like an empty disc.
Disc::Disc(const std::string& path)
    : mFile(std::filesystem::is_regular_file(path) ? std::fopen(path.c_str(), "rb") : nullptr) {}

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

std::optional<DiscFile> Disc::getRoot() {
    std::vector<u8> sector;
    if (!read(kVolumeDescriptorSector, 1, sector)) {
        return std::nullopt;
    }
    const u8* root = sector.data() + kRootRecordOffset;
    const DirectoryRecord record = readRecord(root);
    return DiscFile{record.extent, record.dataLength, ""};
}

std::vector<u8> Disc::readDirectory(const DiscFile& directory) {
    const u32 sectors = (directory.size + kSectorBytes - 1) / kSectorBytes;
    std::vector<u8> data;
    if (sectors == 0 || !read(directory.sector, sectors, data)) {
        return {};
    }
    return data;
}

// The first two records are "." and "..", which a walk would never leave.
std::vector<DiscFile> Disc::list(const DiscFile& directory) {
    const std::vector<u8> data = readDirectory(directory);
    std::vector<DiscFile> entries;
    usize offset = 0;
    while (offset + sizeof(DirectoryRecord) < data.size()) {
        const DirectoryRecord record = readRecord(data.data() + offset);
        if (record.length == 0) {
            offset = (offset / kSectorBytes + 1) * kSectorBytes;
            continue;
        }
        const char* found = nameOf(data.data() + offset);
        if (record.nameLength > 1 || (found[0] != 0 && found[0] != 1)) {
            entries.push_back(DiscFile{record.extent, record.dataLength,
                                       std::string(found, record.nameLength),
                                       (record.fileFlags & kDirectoryFlag) != 0});
        }
        offset += record.length;
    }
    return entries;
}

std::optional<DiscFile> Disc::find(const std::string& path) {
    const std::optional<DiscFile> root = getRoot();
    if (!root) {
        return std::nullopt;
    }
    DiscFile current = *root;

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
    while (offset + sizeof(DirectoryRecord) < data.size()) {
        const DirectoryRecord record = readRecord(data.data() + offset);
        if (record.length == 0) {
            // Records never straddle a sector, so this one is padding to its end.
            offset = (offset / kSectorBytes + 1) * kSectorBytes;
            continue;
        }
        const char* found = nameOf(data.data() + offset);
        if (namesMatch(name, data.data() + offset + sizeof(DirectoryRecord),
                       record.nameLength)) {
            return DiscFile{record.extent, record.dataLength,
                            std::string(found, record.nameLength)};
        }
        offset += record.length;
    }
    return std::nullopt;
}

}  // namespace uee
