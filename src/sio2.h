#pragma once

#include "host.h"
#include "card.h"
#include "pad.h"
#include "types.h"

#include <deque>
#include <optional>
#include <span>

namespace uee {

class IopKernel;

// The bus the pads and the memory cards answer on. SIO2MAN queues one command
// per device in SEND3 and moves the bytes through the two FIFOs.
class Sio2 {
public:
    void attach(IopKernel& kernel) { mKernel = &kernel; }
    void attachPad(PadSource* pad) { mPads[0] = DualShock{pad}; }
    void attachCard(CardStorage* card) { mCards[0] = MemoryCard{card}; }

    std::optional<u32> read(u32 address);
    bool write(u32 address, u32 value);

    // What the SIO2 DMA channels move, rather than a byte at a time.
    void writeFifoIn(std::span<const u8> bytes);
    void readFifoOut(std::span<u8> bytes);
    usize pendingOut() const { return mOut.size(); }

private:
    static constexpr usize kQueued = 16;
    static constexpr usize kPortControls = 8;
    // Ports 0 and 1 are the controllers, 2 and 3 the card slots.
    static constexpr usize kPorts = 2;

    void runQueue();
    void answerCommand(u32 port, usize sent, usize clocked);
    bool answerAsDevice(u32 port, std::span<const u8> command, std::span<u8> response);

    IopKernel* mKernel = nullptr;
    DualShock mPads[kPorts] = {DualShock{nullptr}, DualShock{nullptr}};
    MemoryCard mCards[kPorts] = {MemoryCard{nullptr}, MemoryCard{nullptr}};
    u32 mSend3[kQueued] = {};
    u32 mPortControl[kPortControls] = {};
    u32 mControl = 0;
    u32 mReceive1 = 0;
    u32 mReceive2 = 0;
    u32 mReceive3 = 0;
    u32 mStatus = 0;
    std::deque<u8> mIn;
    std::deque<u8> mOut;
};

}  // namespace uee
