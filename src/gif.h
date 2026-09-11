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
    bool isPacketOpen() const {
        const Packet& open = mPackets[mActivePath & 3u];
        return open.remaining != 0 || !open.endOfPacket;
    }

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
    // The boot rom leaves a path 3 transfer open, which is what a program finds
    // in the output path bits before it starts one of its own.
    static constexpr u32 kOutputPathActive = 1u << 9;

    u32 mStatus = kOutputPathActive;
    u32 mActivePath = 3;
    bool mPath3Masked = false;
    static constexpr u32 kFifoDepth = 16;

    // What a masked path was sent, quadword by quadword, waiting for the mask
    // to lift.
    std::vector<u64> mHeld;
    // Each path walks a packet of its own, so one that arrives in the middle of
    // another does not take over the tag it was counting down.
    struct Packet {
        u32 remaining = 0;
        u32 descriptors = 0;
        u64 registers = 0;
        u32 slot = 0;
        u32 format = 0;
        bool endOfPacket = true;
    };

    Packet mPackets[4];

    Packet& getPacket() { return mPackets[mActivePath & 3u]; }
};

}  // namespace uee
