#include "sio2.h"

#include "iop_kernel.h"
#include "log.h"

namespace uee {
namespace {

LoggerChannel log("sio2");

constexpr u32 kSend3 = 0x00;
constexpr u32 kSend12 = 0x40;
constexpr u32 kFifoIn = 0x60;
constexpr u32 kFifoOut = 0x64;
constexpr u32 kControl = 0x68;
constexpr u32 kReceive1 = 0x6C;
constexpr u32 kReceive2 = 0x70;
constexpr u32 kReceive3 = 0x74;
constexpr u32 kStatus = 0x80;

constexpr u32 kStart = 1u << 0;
constexpr u32 kReset = 1u << 2;

// SIO2MAN: the handler registered on this line collects the results.
constexpr u32 kSio2Interrupt = 17;

// A SEND3 entry is empty once the queue ends; the low bits carry the length.
constexpr u32 kLengthMask = 0x1FFu;
constexpr u32 kLengthShift = 8;

// The pad answers a poll with its id and a marker before the button bytes.
constexpr u8 kAcknowledge = 0xFF;
constexpr u8 kDigitalPad = 0x41;
constexpr u8 kMarker = 0x5A;
constexpr u8 kPollCommand = 0x42;
constexpr u8 kNoDevice = 0xFF;

// SIO2MAN reads this as the device having answered.
constexpr u32 kDeviceAnswered = 0x1000;

}  // namespace

std::optional<u32> Sio2::read(u32 address) {
    if (address - kSend3 < kQueued * 4) {
        return mSend3[(address - kSend3) / 4];
    }
    if (address - kSend12 < kQueued * 4) {
        return mSend12[(address - kSend12) / 4];
    }
    switch (address) {
        case kFifoOut: {
            if (mOut.empty()) {
                return 0;
            }
            const u8 byte = mOut.front();
            mOut.pop_front();
            return byte;
        }
        case kControl: return mControl;
        case kReceive1: return mReceive1;
        case kReceive2: return mReceive2;
        case kReceive3: return mReceive3;
        case kStatus: return mStatus;
        default: return std::nullopt;
    }
}

bool Sio2::write(u32 address, u32 value) {
    if (address - kSend3 < kQueued * 4) {
        mSend3[(address - kSend3) / 4] = value;
        return true;
    }
    if (address - kSend12 < kQueued * 4) {
        mSend12[(address - kSend12) / 4] = value;
        return true;
    }
    switch (address) {
        case kFifoIn: mIn.push_back(static_cast<u8>(value)); return true;
        case kControl:
            mControl = value;
            if ((value & kReset) != 0) {
                mIn.clear();
                mOut.clear();
            }
            if ((value & kStart) != 0) {
                runQueue();
            }
            return true;
        case kStatus: mStatus &= ~value; return true;
        default: return false;
    }
}

void Sio2::runQueue() {
    mOut.clear();
    mReceive1 = 0;
    for (const u32 entry : mSend3) {
        if (entry == 0) {
            break;
        }
        answerCommand((entry >> kLengthShift) & kLengthMask);
    }
    mIn.clear();
    mReceive3 = static_cast<u32>(mOut.size());
    mControl &= ~kStart;
    mStatus |= 1;
    log.trace("queue answered {} bytes, receive1 {:08x}", mOut.size(), mReceive1);
    mKernel->raiseInterrupt(kSio2Interrupt);
}

// The command's second byte selects what the device is being asked for. Only a
// poll is answered; anything else reads as a port with nothing plugged in.
void Sio2::answerCommand(u32 length) {
    const u8 command = mIn.size() > 1 ? mIn[1] : 0;
    if (mPad == nullptr || command != kPollCommand) {
        for (u32 i = 0; i < length; ++i) {
            mOut.push_back(kNoDevice);
        }
        return;
    }

    const u16 buttons = mPad->buttons();
    for (u32 i = 0; i < length; ++i) {
        switch (i) {
            case 0: mOut.push_back(kAcknowledge); break;
            case 1: mOut.push_back(kDigitalPad); break;
            case 2: mOut.push_back(kMarker); break;
            case 3: mOut.push_back(static_cast<u8>(buttons >> 8)); break;
            case 4: mOut.push_back(static_cast<u8>(buttons)); break;
            default: mOut.push_back(0); break;
        }
    }
    mReceive1 = kDeviceAnswered;
}

}  // namespace uee
