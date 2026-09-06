#pragma once

#include "types.h"

#include <memory>
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
    virtual ~Disc() = default;

    virtual bool isOpen() const = 0;
    virtual bool read(u32 sector, u32 count, std::vector<u8>& out) = 0;
    virtual std::optional<DiscFile> find(const std::string& path) = 0;
    virtual std::vector<DiscFile> list(const DiscFile& directory) = 0;
    virtual std::optional<DiscFile> getRoot() = 0;
};

// An image, or a directory holding what one was extracted to. Never null: a
// path naming neither answers isOpen with false.
std::unique_ptr<Disc> openDisc(const std::string& path);

}  // namespace uee
