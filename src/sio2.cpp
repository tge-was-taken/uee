// A `Sio2*` tag names the routine that settles a detail, and a `Pad*` or `Mc2*`
// one a routine in the module that drives the device on the other end:
//   SIO2MAN md5 ea22447abe3a4b56a26f3149316370a3
//   PADMAN  md5 0fa9dbfe8794e172733189ed945aff45
//   MC2_S1  md5 c493ecb797ddd06b4ebd7eee6aff1959

#include "sio2.h"

#include "iop_kernel.h"
#include "log.h"

#include <algorithm>
#include <format>
#include <string>
#include <vector>

namespace uee {
namespace {

LoggerChannel log("sio2");

constexpr u32 kSend3 = 0x00;
constexpr u32 kPortControl = 0x40;
constexpr u32 kFifoIn = 0x60;
constexpr u32 kFifoOut = 0x64;
constexpr u32 kControl = 0x68;
constexpr u32 kReceive1 = 0x6C;
constexpr u32 kReceive2 = 0x70;
constexpr u32 kReceive3 = 0x74;
constexpr u32 kStatus = 0x80;

// Sio2TransferStart @ 0x2f0 sets bit 0, Sio2FifoReset @ 0x1a4 the other two.
constexpr u32 kStart = 1u << 0;
constexpr u32 kResetIn = 1u << 2;
constexpr u32 kResetOut = 1u << 3;

// SIO2MAN's start @ 0x3c4 registers its handler on this line.
constexpr u32 kSio2Interrupt = 17;

// Mc2Sio2QueueCommand @ 0x7c4 packs an entry: the port, then the length of the
// command, then the length of the answer. The transfer clocks the longer one,
// and a zero entry ends the queue.
constexpr u32 kPortMask = 0x3;
constexpr u32 kLengthMask = 0x1FF;
constexpr u32 kSendShift = 8;
constexpr u32 kReceiveShift = 18;

// PadSlotStateAdvance @ 0x4c4c gives up on a slot when bit 13 of RECV1 is set,
// which an empty port is not: it answers with an idle line.
constexpr u32 kDeviceAnswered = 0x1100;
constexpr u32 kNoDevice = 0x1D100;
constexpr u8 kIdle = 0xFF;

// The address that leads a command: PadCmd42Payload5 @ 0x66d0 sends 0x01 and
// Mc2PackProbe @ 0xa20 sends 0x81.
constexpr u8 kController = 0x01;
constexpr u8 kCard = 0x81;

std::string toHex(std::span<const u8> bytes) {
    std::string text;
    for (const u8 byte : bytes) {
        text += std::format("{:02x} ", byte);
    }
    return text;
}

}  // namespace

std::optional<u32> Sio2::read(u32 address) {
    if (address - kSend3 < kQueued * 4) {
        return mSend3[(address - kSend3) / 4];
    }
    if (address - kPortControl < kPortControls * 4) {
        return mPortControl[(address - kPortControl) / 4];
    }
    switch (address) {
        case kFifoOut: {
            if (mOut.empty()) {
                return kIdle;
            }
            const u8 byte = mOut.front();
            mOut.pop_front();
            return byte;
        }
        case kControl: return mControl;
        case kReceive1: return mReceive1;
        // PADMAN: PadTransferStart @ 0x5680 reads two bits out of RECV2 that
        // nothing here sets.
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
    if (address - kPortControl < kPortControls * 4) {
        mPortControl[(address - kPortControl) / 4] = value;
        return true;
    }
    switch (address) {
        case kFifoIn: mIn.push_back(static_cast<u8>(value)); return true;
        case kControl: {
            // SIO2MAN leaves the reset bits set once Sio2FifoReset @ 0x1a4 has
            // written them, so only the write that raises one empties a FIFO.
            const u32 raised = value & ~mControl;
            mControl = value;
            if ((raised & kResetIn) != 0) {
                mIn.clear();
            }
            if ((raised & kResetOut) != 0) {
                mOut.clear();
            }
            if ((value & kStart) != 0) {
                runQueue();
            }
            return true;
        }
        case kStatus: mStatus &= ~value; return true;
        default: return false;
    }
}

void Sio2::writeFifoIn(std::span<const u8> bytes) {
    mIn.insert(mIn.end(), bytes.begin(), bytes.end());
}

void Sio2::readFifoOut(std::span<u8> bytes) {
    for (u8& byte : bytes) {
        byte = mOut.empty() ? kIdle : mOut.front();
        if (!mOut.empty()) {
            mOut.pop_front();
        }
    }
}

void Sio2::runQueue() {
    mOut.clear();
    for (const u32 entry : mSend3) {
        if (entry == 0) {
            break;
        }
        const usize sent = (entry >> kSendShift) & kLengthMask;
        const usize received = (entry >> kReceiveShift) & kLengthMask;
        answerCommand(entry & kPortMask, sent, std::max(sent, received));
    }
    mIn.clear();
    mReceive3 = static_cast<u32>(mOut.size());
    mControl &= ~kStart;
    mStatus |= 1;
    mKernel->raiseInterrupt(kSio2Interrupt);
}

bool Sio2::answerAsDevice(u32 port, std::span<const u8> command, std::span<u8> response) {
    if (command.empty()) {
        return false;
    }
    if (port < kPorts && command[0] == kController && mPads[port].isConnected()) {
        mPads[port].answer(command, response);
        return true;
    }
    const u32 slot = port - kPorts;
    if (slot < kPorts && command[0] == kCard && mCards[slot].isPresent()) {
        mCards[slot].answer(command, response);
        return true;
    }
    return false;
}

// A command's bytes are taken even when nothing answers it, since the next
// entry's follow them in the same FIFO.
void Sio2::answerCommand(u32 port, usize sent, usize clocked) {
    std::vector<u8> command(sent, 0);
    for (u8& byte : command) {
        if (mIn.empty()) {
            break;
        }
        byte = mIn.front();
        mIn.pop_front();
    }

    std::vector<u8> response(clocked, kIdle);
    const bool answered = answerAsDevice(port, command, response);
    mReceive1 = answered ? kDeviceAnswered : kNoDevice;
    if (log.tracing()) {
        log.trace("port {} sent {}got {}", port, toHex(command), toHex(response));
    }
    mOut.insert(mOut.end(), response.begin(), response.end());
}

}  // namespace uee
