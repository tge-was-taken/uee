#pragma once

#include "gs.h"
#include "types.h"

#include <optional>

namespace uee {

class Memory;

class Gif {
public:
    void attach(Memory& memory, Gs& gs) {
        mMemory = &memory;
        mGs = &gs;
    }

    void writeQuadword(u64 low, u64 high);

    std::optional<u32> read(u32 address) const;
    bool write(u32 address, u32 value);

    // The VIF reports the third path's mask through the GIF's own status.
    void maskPath3(bool masked);

private:
    void readTag(u64 low, u64 high);
    void writeRegister(u32 address, u64 value);

    Memory* mMemory = nullptr;
    Gs* mGs = nullptr;
    u32 mControl = 0;
    u32 mMode = 0;
    u32 mStatus = 0;
    u32 mRemaining = 0;
    u32 mDescriptors = 0;
    u64 mRegisters = 0;
    u32 mSlot = 0;
    u32 mFormat = 0;
};

}  // namespace uee
