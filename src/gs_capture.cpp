#include "gs_capture.h"

#include <format>
#include <stdexcept>

namespace uee {
namespace {

constexpr char kMagic[8] = {'U', 'E', 'E', 'G', 'S', 'C', 'A', 'P'};
constexpr u32 kVersion = 1;
constexpr u32 kPrivRegisterBytes = 8192;

constexpr u8 kRegisterRecord = 1;
constexpr u8 kTransferRecord = 2;
constexpr u8 kFieldRecord = 3;

void checkRegisterSize(std::span<const u8> registers) {
    if (registers.size() != kPrivRegisterBytes) {
        throw std::runtime_error(
            std::format("a snapshot carried {} bytes of registers, not {}",
                        registers.size(), kPrivRegisterBytes));
    }
}

}  // namespace

GsCaptureWriter::GsCaptureWriter(std::filesystem::path path, std::string serial)
    : mFile(path, std::ios::binary), mSerial(std::move(serial)) {}

void GsCaptureWriter::writeByte(u8 value) {
    mFile.put(static_cast<char>(value));
}

void GsCaptureWriter::writeWord(u32 value) {
    for (u32 shift = 0; shift < 32; shift += 8) {
        writeByte(static_cast<u8>(value >> shift));
    }
}

void GsCaptureWriter::writeBytes(std::span<const u8> bytes) {
    mFile.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
}

void GsCaptureWriter::begin(std::span<const u8> registers) {
    checkRegisterSize(registers);
    writeBytes({reinterpret_cast<const u8*>(kMagic), sizeof(kMagic)});
    writeWord(kVersion);
    writeWord(kPrivRegisterBytes);
    writeWord(static_cast<u32>(mSerial.size()));
    writeBytes({reinterpret_cast<const u8*>(mSerial.data()), mSerial.size()});

    writeByte(kRegisterRecord);
    writeBytes(registers);
}

void GsCaptureWriter::transfer(u32 path, std::span<const u8> data) {
    if (data.empty()) {
        return;
    }
    writeByte(kTransferRecord);
    writeByte(static_cast<u8>(path));
    writeWord(static_cast<u32>(data.size()));
    writeBytes(data);
}

void GsCaptureWriter::vsync(u32 field, std::span<const u8> registers) {
    checkRegisterSize(registers);
    writeByte(kRegisterRecord);
    writeBytes(registers);
    writeByte(kFieldRecord);
    writeByte(static_cast<u8>(field));
}

}  // namespace uee
