#pragma once

#include "host.h"
#include "types.h"

#include <span>

namespace uee {

// The memory card side of the SIO2 protocol. MC2_S1 identifies a card before
// it reads one, and a slot that misses any step of that is reported empty.
class MemoryCard {
public:
    explicit MemoryCard(CardStorage* storage) : mStorage(storage) {}

    bool isPresent() const { return mStorage != nullptr; }
    void answer(std::span<const u8> command, std::span<u8> response);

private:
    void reportSpecs(std::span<u8> data);
    void reportTerminator(std::span<u8> data);
    void setTerminator(u8 value);
    void setPage(std::span<const u8> payload);
    void readData(std::span<u8> data, usize count);
    void writeData(std::span<const u8> payload, usize count);

    CardStorage* mStorage = nullptr;
    u8 mTerminator = 0x55;
    u32 mPage = 0;
    u32 mOffset = 0;
};

}  // namespace uee
