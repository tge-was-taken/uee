#pragma once

#include "host.h"
#include "pad.h"
#include "types.h"

#include <deque>
#include <optional>
#include <span>

namespace uee {

class IopKernel;

// SIO2MAN md5 ea22447abe3a4b56a26f3149316370a3 drives the pads and the memory
// cards through this. Sio2TransferSubmit @ 0x1c4 fills SEND3 with one entry per
// command, pushes the command bytes in through one FIFO, and Sio2IntrHandler @
// 0x384 collects the answers out of the other.
class Sio2 {
public:
    void attach(IopKernel& kernel) { mKernel = &kernel; }
    void attachPad(PadSource* pad) { mPads[0] = DualShock{pad}; }

    std::optional<u32> read(u32 address);
    bool write(u32 address, u32 value);

    // What the SIO2 DMA channels move when a module drives the FIFOs that way
    // rather than a byte at a time.
    void writeFifoIn(std::span<const u8> bytes);
    void readFifoOut(std::span<u8> bytes);
    usize pendingOut() const { return mOut.size(); }

private:
    static constexpr usize kQueued = 16;
    static constexpr usize kPortControls = 8;
    static constexpr usize kPorts = 2;

    void runQueue();
    void answerCommand(u32 port, usize length);

    IopKernel* mKernel = nullptr;
    DualShock mPads[kPorts] = {DualShock{nullptr}, DualShock{nullptr}};
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
