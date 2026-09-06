#pragma once

#include "host.h"
#include "types.h"

#include <filesystem>
#include <fstream>
#include <span>
#include <string>

namespace uee {

// The graphics stream as this runtime saw it, in a file of its own. Nothing
// here draws it; tools/gsdump turns it into what an emulator replays.
class GsCaptureWriter : public GsHandler {
public:
    GsCaptureWriter(std::filesystem::path path, std::string serial);

    bool isOpen() const { return mFile.is_open(); }

    void begin(std::span<const u8> registers) override;
    void transfer(u32 path, std::span<const u8> data) override;
    void vsync(u32 field, std::span<const u8> registers) override;

private:
    void writeByte(u8 value);
    void writeWord(u32 value);
    void writeBytes(std::span<const u8> bytes);

    std::ofstream mFile;
    std::string mSerial;
};

}  // namespace uee
