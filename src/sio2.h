#pragma once

#include "host.h"
#include "card.h"
#include "pad.h"
#include "types.h"

#include <optional>
#include <span>
#include <vector>

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

    // The DMA gives each queued command a block of its own, where a driver
    // writing the FIFO a byte at a time packs them end to end.
    void writeFifoIn(std::span<const u8> bytes, u32 blockBytes);
    void readFifoOut(std::span<u8> bytes, u32 blockBytes);
    bool hasAnswers() const { return !mAnswerStarts.empty(); }

private:
    static constexpr usize kQueued = 16;
    static constexpr usize kPortControls = 8;
    // Ports 0 and 1 are the controllers, 2 and 3 the card slots.
    static constexpr usize kPorts = 2;

    void runQueue();
    bool answerCommand(u32 port, std::span<const u8> command, usize clocked);
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
    std::vector<u8> mIn;
    std::vector<u8> mOut;
    std::vector<u32> mAnswerStarts;
    u32 mInBlock = 0;
    usize mOutRead = 0;
};

}  // namespace uee
