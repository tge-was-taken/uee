#pragma once

#include "host.h"
#include "types.h"

#include <deque>
#include <optional>

namespace uee {

class IopKernel;

// SIO2MAN md5 ea22447abe3a4b56a26f3149316370a3 drives the pads through this. A
// queued command is described by a SEND3 entry, its bytes go in through one
// FIFO and the device's answer comes back out of the other.
class Sio2 {
public:
    void attach(IopKernel& kernel) { mKernel = &kernel; }
    void attachPad(PadSource* pad) { mPad = pad; }

    std::optional<u32> read(u32 address);
    bool write(u32 address, u32 value);

private:
    static constexpr usize kQueued = 16;

    void runQueue();
    void answerCommand(u32 length);

    IopKernel* mKernel = nullptr;
    PadSource* mPad = nullptr;
    u32 mSend3[kQueued] = {};
    u32 mSend12[kQueued] = {};
    u32 mControl = 0;
    u32 mReceive1 = 0;
    u32 mReceive2 = 0;
    u32 mReceive3 = 0;
    u32 mStatus = 0;
    std::deque<u8> mIn;
    std::deque<u8> mOut;
};

}  // namespace uee
