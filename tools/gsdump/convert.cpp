// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The layout is PCSX2's, read out of its GSDump.cpp and GSLzma.cpp, so this
// program carries PCSX2's licence and links nothing of this runtime's.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;

constexpr char kCaptureMagic[8] = {'U', 'E', 'E', 'G', 'S', 'C', 'A', 'P'};
constexpr u32 kCaptureVersion = 1;
constexpr u8 kCaptureRegisters = 1;
constexpr u8 kCaptureTransfer = 2;
constexpr u8 kCaptureField = 3;

constexpr u32 kNewHeaderMarker = 0xFFFFFFFFu;
constexpr u32 kHeaderWords = 9;
constexpr u32 kStateVersion = 9;
constexpr u32 kPrivRegisterBytes = 8192;

// GSState::Defrost refuses a state shorter than the version it reads asks for
// and ignores whatever follows, so a state longer than any layout it has needs
// no field of the layout restated here.
constexpr u32 kStateBytes = 4 * 1024 * 1024 + 4096;

constexpr u8 kTransferRecord = 0;
constexpr u8 kVsyncRecord = 1;
constexpr u8 kRegisterRecord = 3;

class Reader {
public:
    explicit Reader(std::vector<u8> bytes) : mBytes(std::move(bytes)) {}

    bool atEnd() const { return mPosition >= mBytes.size(); }

    u8 byte() { return take(1)[0]; }

    u32 word() {
        const std::span<const u8> bytes = take(4);
        return bytes[0] | (u32(bytes[1]) << 8) | (u32(bytes[2]) << 16) | (u32(bytes[3]) << 24);
    }

    std::span<const u8> take(std::size_t count) {
        if (mPosition + count > mBytes.size()) {
            std::fprintf(stderr, "capture ends inside a record\n");
            std::exit(1);
        }
        const std::span<const u8> bytes(mBytes.data() + mPosition, count);
        mPosition += count;
        return bytes;
    }

private:
    std::vector<u8> mBytes;
    std::size_t mPosition = 0;
};

class Writer {
public:
    explicit Writer(std::FILE* file) : mFile(file) {}

    void byte(u8 value) { std::fputc(value, mFile); }

    void word(u32 value) {
        for (u32 shift = 0; shift < 32; shift += 8) {
            byte(static_cast<u8>(value >> shift));
        }
    }

    void bytes(std::span<const u8> data) {
        std::fwrite(data.data(), 1, data.size(), mFile);
    }

    void zeroes(u32 count) {
        const std::array<u8, 4096> block{};
        while (count != 0) {
            const u32 chunk = count < block.size() ? count : u32(block.size());
            std::fwrite(block.data(), 1, chunk, mFile);
            count -= chunk;
        }
    }

private:
    std::FILE* mFile;
};

std::vector<u8> readFile(const char* path) {
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        std::fprintf(stderr, "cannot open %s\n", path);
        std::exit(1);
    }
    std::vector<u8> bytes;
    std::array<u8, 1 << 16> block{};
    for (;;) {
        const std::size_t read = std::fread(block.data(), 1, block.size(), file);
        bytes.insert(bytes.end(), block.begin(), block.begin() + read);
        if (read != block.size()) {
            break;
        }
    }
    std::fclose(file);
    return bytes;
}

void writeHead(Writer& out, const std::string& serial, std::span<const u8> registers) {
    const u32 headerSize = kHeaderWords * 4 + u32(serial.size());
    out.word(kNewHeaderMarker);
    out.word(headerSize);
    out.word(kStateVersion);
    out.word(kStateBytes);
    out.word(kHeaderWords * 4);
    out.word(u32(serial.size()));
    for (u32 remaining = 0; remaining < 5; ++remaining) {
        out.word(0);
    }
    out.bytes({reinterpret_cast<const u8*>(serial.data()), serial.size()});

    out.word(kStateVersion);
    out.zeroes(kStateBytes - 4);
    out.bytes(registers);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <capture.ueegs> <dump.gs>\n", argv[0]);
        return 2;
    }

    Reader in(readFile(argv[1]));
    if (std::memcmp(in.take(sizeof(kCaptureMagic)).data(), kCaptureMagic,
                    sizeof(kCaptureMagic)) != 0) {
        std::fprintf(stderr, "%s is not a capture\n", argv[1]);
        return 1;
    }
    if (in.word() != kCaptureVersion) {
        std::fprintf(stderr, "%s was written in another version\n", argv[1]);
        return 1;
    }
    const u32 registerBytes = in.word();
    if (registerBytes != kPrivRegisterBytes) {
        std::fprintf(stderr, "%s spends %u bytes on registers, not %u\n", argv[1],
                     registerBytes, kPrivRegisterBytes);
        return 1;
    }
    const u32 serialSize = in.word();
    const std::span<const u8> serialBytes = in.take(serialSize);
    const std::string serial(serialBytes.begin(), serialBytes.end());

    std::FILE* file = std::fopen(argv[2], "wb");
    if (file == nullptr) {
        std::fprintf(stderr, "cannot write %s\n", argv[2]);
        return 1;
    }
    Writer out(file);

    bool opened = false;
    while (!in.atEnd()) {
        switch (in.byte()) {
            case kCaptureRegisters: {
                const std::span<const u8> registers = in.take(registerBytes);
                if (!opened) {
                    writeHead(out, serial, registers);
                    opened = true;
                    break;
                }
                out.byte(kRegisterRecord);
                out.bytes(registers);
                break;
            }
            case kCaptureTransfer: {
                const u8 path = in.byte();
                const u32 size = in.word();
                out.byte(kTransferRecord);
                out.byte(path);
                out.word(size);
                out.bytes(in.take(size));
                break;
            }
            case kCaptureField:
                out.byte(kVsyncRecord);
                out.byte(in.byte());
                break;
            default:
                std::fprintf(stderr, "%s holds a record this does not know\n", argv[1]);
                std::fclose(file);
                return 1;
        }
    }

    std::fclose(file);
    return 0;
}
