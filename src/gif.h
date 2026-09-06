#pragma once

#include "gs.h"
#include "types.h"

#include <optional>
#include <vector>

namespace uee {

class GsHandler;
class Memory;

class Gif {
public:
    void attach(Memory& memory, Gs& gs) {
        mMemory = &memory;
        mGs = &gs;
    }

    void attachHandler(GsHandler* handler) { mHandler = handler; }

    void writeQuadword(u64 low, u64 high);

    // A kicked packet names no length, so whoever feeds one asks after every
    // quadword whether the tag that ends it has arrived.
    bool isPacketOpen() const { return mRemaining != 0 || !mEndOfPacket; }

    // Idle while claiming a queued quadword is a state the hardware cannot be
    // in, and a handler waiting on either never gets its answer.
    void beginPath(u32 path);
    void endPath();

    std::optional<u32> read(u32 address) const;
    bool write(u32 address, u32 value);

    // The VIF reports the third path's mask through the GIF's own status.
    void maskPath3(bool masked);

private:
    void readTag(u64 low, u64 high);
    void writeRegister(u32 address, u64 value);
    void capture(u64 low, u64 high);
    void flushCapture();

    Memory* mMemory = nullptr;
    Gs* mGs = nullptr;
    GsHandler* mHandler = nullptr;
    std::vector<u8> mCaptured;
    u32 mCapturedPath = 0;
    u32 mControl = 0;
    u32 mMode = 0;
    u32 mStatus = 0;
    u32 mActivePath = 0;
    u32 mQueued = 0;
    u32 mRemaining = 0;
    u32 mDescriptors = 0;
    u64 mRegisters = 0;
    u32 mSlot = 0;
    u32 mFormat = 0;
    bool mEndOfPacket = true;
};

}  // namespace uee
