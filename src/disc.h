#pragma once

#include "types.h"

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace uee {

constexpr u32 kSectorBytes = 2048;

struct DiscFile {
    u32 sector = 0;
    u32 size = 0;
    std::string name;
    bool isDirectory = false;
};

// The directory walk is here because the drive answers a name lookup itself.
class Disc {
public:
    explicit Disc(const std::string& path);

    bool isOpen() const { return mFile != nullptr; }
    ~Disc();

    Disc(const Disc&) = delete;
    Disc& operator=(const Disc&) = delete;

    bool read(u32 sector, u32 count, std::vector<u8>& out);
    std::optional<DiscFile> find(const std::string& path);
    std::vector<DiscFile> list(const DiscFile& directory);
    std::optional<DiscFile> getRoot();

private:
    std::optional<DiscFile> findInDirectory(const DiscFile& directory, const std::string& name);
    std::vector<u8> readDirectory(const DiscFile& directory);

    std::FILE* mFile = nullptr;
};

}  // namespace uee
