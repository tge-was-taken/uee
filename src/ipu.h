#pragma once

#include "types.h"

#include <optional>
#include <span>
#include <vector>

namespace uee {

// A pipe, not a decoder: nothing here knows where a picture ends, so what a
// reader does not take waits for the next one.
class Ipu {
public:
    void receive(std::span<const u8> bytes);
    void consume(usize bytes);

    std::span<const u8> getStream() const { return mStream; }

    std::optional<u32> read(u32 address) const;
    bool write(u32 address, u32 value);

private:
    u32 mControl = 0;
    u32 mBitPosition = 0;
    u32 mDecoded = 0;
    std::vector<u8> mStream;
};

}  // namespace uee
