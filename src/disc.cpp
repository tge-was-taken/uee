#include "disc.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
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

class IsoDisc final : public Disc {
public:
    explicit IsoDisc(const std::string& path);
    ~IsoDisc() override;

    IsoDisc(const IsoDisc&) = delete;
    IsoDisc& operator=(const IsoDisc&) = delete;

    bool isOpen() const override { return mFile != nullptr; }
    bool read(u32 sector, u32 count, std::vector<u8>& out) override;
    std::optional<DiscFile> find(const std::string& path) override;
    std::vector<DiscFile> list(const DiscFile& directory) override;
    std::optional<DiscFile> getRoot() override;

private:
    std::optional<DiscFile> findInDirectory(const DiscFile& directory, const std::string& name);
    std::vector<u8> readDirectory(const DiscFile& directory);

    std::FILE* mFile = nullptr;
};

// Past the volume descriptors, so a sector a guest reports looks like one.
constexpr u32 kFirstSector = 24;

// A directory an image was extracted to. The guest still asks by sector, so
// every file is given a run of them and a read lands in whichever run holds it.
class FolderDisc final : public Disc {
public:
    explicit FolderDisc(const std::filesystem::path& root);
    ~FolderDisc() override;

    FolderDisc(const FolderDisc&) = delete;
    FolderDisc& operator=(const FolderDisc&) = delete;

    bool isOpen() const override { return !mEntries.empty(); }
    bool read(u32 sector, u32 count, std::vector<u8>& out) override;
    std::optional<DiscFile> find(const std::string& path) override;
    std::vector<DiscFile> list(const DiscFile& directory) override;
    std::optional<DiscFile> getRoot() override;

private:
    struct Entry {
        std::filesystem::path path;
        std::string name;
        u32 sector = 0;
        u32 size = 0;
        bool isDirectory = false;
        std::vector<usize> children;
    };

    usize add(const std::filesystem::path& path, const std::string& name, bool isDirectory);
    void walk(usize directory);
    const Entry* entryAt(u32 sector) const;
    std::FILE* openFor(const Entry& entry);
    DiscFile fileOf(const Entry& entry) const {
        return DiscFile{entry.sector, entry.size, entry.name, entry.isDirectory};
    }

    std::vector<Entry> mEntries;
    u32 mNextSector = kFirstSector;
    u32 mOpenSector = 0;
    std::FILE* mOpenFile = nullptr;
};

}  // namespace

// Reads fail after the open succeeds, which looks like an empty disc.
IsoDisc::IsoDisc(const std::string& path)
    : mFile(std::filesystem::is_regular_file(path) ? std::fopen(path.c_str(), "rb") : nullptr) {}

IsoDisc::~IsoDisc() {
    if (mFile != nullptr) {
        std::fclose(mFile);
    }
}

bool IsoDisc::read(u32 sector, u32 count, std::vector<u8>& out) {
    if (mFile == nullptr || count > kSectorLimit) {
        return false;
    }
    out.assign(static_cast<usize>(count) * kSectorBytes, 0);
    if (std::fseek(mFile, static_cast<long>(sector) * kSectorBytes, SEEK_SET) != 0) {
        return false;
    }
    return std::fread(out.data(), 1, out.size(), mFile) == out.size();
}

std::optional<DiscFile> IsoDisc::getRoot() {
    std::vector<u8> sector;
    if (!read(kVolumeDescriptorSector, 1, sector)) {
        return std::nullopt;
    }
    const u8* root = sector.data() + kRootRecordOffset;
    const DirectoryRecord record = readRecord(root);
    return DiscFile{record.extent, record.dataLength, ""};
}

std::vector<u8> IsoDisc::readDirectory(const DiscFile& directory) {
    const u32 sectors = (directory.size + kSectorBytes - 1) / kSectorBytes;
    std::vector<u8> data;
    if (sectors == 0 || !read(directory.sector, sectors, data)) {
        return {};
    }
    return data;
}

// The first two records are "." and "..", which a walk would never leave.
std::vector<DiscFile> IsoDisc::list(const DiscFile& directory) {
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

std::optional<DiscFile> IsoDisc::find(const std::string& path) {
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

std::optional<DiscFile> IsoDisc::findInDirectory(const DiscFile& directory,
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

usize FolderDisc::add(const std::filesystem::path& path, const std::string& name,
                      bool isDirectory) {
    Entry entry;
    entry.path = path;
    entry.name = name;
    entry.isDirectory = isDirectory;
    if (!isDirectory) {
        std::error_code failed;
        const auto size = std::filesystem::file_size(path, failed);
        entry.size = failed ? 0 : static_cast<u32>(size);
    }
    entry.sector = mNextSector;
    const u32 sectors = (entry.size + kSectorBytes - 1) / kSectorBytes;
    mNextSector += sectors == 0 ? 1 : sectors;
    mEntries.push_back(std::move(entry));
    return mEntries.size() - 1;
}

// Sorted, so the order a guest sees does not depend on the host filesystem.
void FolderDisc::walk(usize directory) {
    std::vector<std::filesystem::directory_entry> found;
    std::error_code failed;
    for (const auto& entry : std::filesystem::directory_iterator(mEntries[directory].path, failed)) {
        found.push_back(entry);
    }
    std::sort(found.begin(), found.end(), [](const auto& left, const auto& right) {
        return left.path().filename() < right.path().filename();
    });

    for (const auto& entry : found) {
        const usize child =
            add(entry.path(), entry.path().filename().string(), entry.is_directory());
        mEntries[directory].children.push_back(child);
    }
    for (const usize child : mEntries[directory].children) {
        if (mEntries[child].isDirectory) {
            walk(child);
        }
    }
}

FolderDisc::FolderDisc(const std::filesystem::path& root) {
    if (!std::filesystem::is_directory(root)) {
        return;
    }
    add(root, "", true);
    walk(0);
}

const FolderDisc::Entry* FolderDisc::entryAt(u32 sector) const {
    for (const Entry& entry : mEntries) {
        const u32 sectors = (entry.size + kSectorBytes - 1) / kSectorBytes;
        if (sector - entry.sector < (sectors == 0 ? 1 : sectors)) {
            return &entry;
        }
    }
    return nullptr;
}

FolderDisc::~FolderDisc() {
    if (mOpenFile != nullptr) {
        std::fclose(mOpenFile);
    }
}

// The guest reads a file through before moving on, and on a mounted host
// filesystem an open costs more than the read does.
std::FILE* FolderDisc::openFor(const Entry& entry) {
    if (mOpenSector == entry.sector) {
        return mOpenFile;
    }
    if (mOpenFile != nullptr) {
        std::fclose(mOpenFile);
    }
    mOpenFile = std::fopen(entry.path.c_str(), "rb");
    mOpenSector = mOpenFile == nullptr ? 0 : entry.sector;
    return mOpenFile;
}

// A read may run past the file it started in, which on a disc would reach the
// next one. Nothing here lays files out end to end, so it reads as blank.
bool FolderDisc::read(u32 sector, u32 count, std::vector<u8>& out) {
    if (mEntries.empty() || count > kSectorLimit) {
        return false;
    }
    const Entry* entry = entryAt(sector);
    if (entry == nullptr || entry->isDirectory) {
        return false;
    }
    std::FILE* file = openFor(*entry);
    if (file == nullptr) {
        return false;
    }
    out.assign(static_cast<usize>(count) * kSectorBytes, 0);

    const long at = static_cast<long>(sector - entry->sector) * kSectorBytes;
    if (std::fseek(file, at, SEEK_SET) != 0) {
        return false;
    }
    std::fread(out.data(), 1, out.size(), file);
    return true;
}

std::optional<DiscFile> FolderDisc::getRoot() {
    if (mEntries.empty()) {
        return std::nullopt;
    }
    return fileOf(mEntries[0]);
}

std::vector<DiscFile> FolderDisc::list(const DiscFile& directory) {
    const Entry* found = entryAt(directory.sector);
    if (found == nullptr || !found->isDirectory) {
        return {};
    }
    std::vector<DiscFile> entries;
    for (const usize child : found->children) {
        entries.push_back(fileOf(mEntries[child]));
    }
    return entries;
}

std::optional<DiscFile> FolderDisc::find(const std::string& path) {
    if (mEntries.empty()) {
        return std::nullopt;
    }
    usize at = 0;
    for (const std::string& part : splitPath(path)) {
        const auto matches = [&](usize child) {
            const std::string& name = mEntries[child].name;
            return namesMatch(part, reinterpret_cast<const u8*>(name.data()),
                              static_cast<u32>(name.size()));
        };
        const auto& children = mEntries[at].children;
        const auto child = std::find_if(children.begin(), children.end(), matches);
        if (child == children.end()) {
            return std::nullopt;
        }
        at = *child;
    }
    return fileOf(mEntries[at]);
}

std::unique_ptr<Disc> openDisc(const std::string& path) {
    if (std::filesystem::is_directory(path)) {
        return std::make_unique<FolderDisc>(path);
    }
    return std::make_unique<IsoDisc>(path);
}

}  // namespace uee
