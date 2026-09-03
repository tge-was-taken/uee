#pragma once

#include "host.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace uee {

// An unwritten page reads all ones, the erased card the driver expects.
class CardFile : public CardStorage {
public:
    explicit CardFile(const std::string& path) : mFile(std::fopen(path.c_str(), "r+b")) {
        if (mFile == nullptr) {
            mFile = std::fopen(path.c_str(), "w+b");
        }
    }

    ~CardFile() override {
        if (mFile != nullptr) {
            std::fclose(mFile);
        }
    }

    CardFile(const CardFile&) = delete;
    CardFile& operator=(const CardFile&) = delete;

    bool isOpen() const { return mFile != nullptr; }

    void readPage(u32 page, std::span<u8> out) override {
        std::fill(out.begin(), out.end(), 0xFF);
        if (std::fseek(mFile, offsetOf(page), SEEK_SET) != 0) {
            return;
        }
        std::fread(out.data(), 1, out.size(), mFile);
    }

    void writePage(u32 page, std::span<const u8> in) override {
        if (std::fseek(mFile, offsetOf(page), SEEK_SET) != 0) {
            return;
        }
        std::fwrite(in.data(), 1, in.size(), mFile);
        std::fflush(mFile);
    }

private:
    static long offsetOf(u32 page) {
        return static_cast<long>(page) * (kCardPageBytes + kCardSpareBytes);
    }

    std::FILE* mFile = nullptr;
};

}  // namespace uee
