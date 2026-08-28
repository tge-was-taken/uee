// SIFCMD md5 bcdaf8bead75a7362a82c95bbbf479c7, SIFMAN md5
// 35a6ac86ca2d7ee9c5a27de8328c3267, SLUS_216.21 md5 9074ba25f5a100940ed233c9f8f675ab.

#include "sif.h"

#include "syscall_stubs.h"

#include <format>
#include <utility>

namespace uee {
namespace {

constexpr u32 kTransferSize = 16;

// SLUS-216.21: sceSifSendCmd @ 0x5064f8 marks only the command packet with this.
constexpr u32 kInterruptOnSend = 0x4;

constexpr u32 kHeaderSize = 0;
constexpr u32 kHeaderCommand = 2;
constexpr u32 kHeaderOption = 3;

constexpr u32 kInitBuffer = 4;

constexpr u32 kRequestRecordId = 4;
constexpr u32 kRequestPacket = 5;
constexpr u32 kRequestRpcId = 6;
constexpr u32 kRequestClient = 7;

constexpr u32 kBindServerId = 8;

constexpr u32 kCallNumber = 8;
constexpr u32 kCallReceiveBuffer = 10;
constexpr u32 kCallReceiveSize = 11;
constexpr u32 kCallServer = 13;

constexpr u32 kReplyCommand = 8;
constexpr u32 kReplyServer = 9;
constexpr u32 kReplyBuffer = 10;
constexpr u32 kReplyWords = 16;

constexpr u32 kRegisterWords = 6;
constexpr u32 kRegisterIndex = 4;
constexpr u32 kRegisterValue = 5;
constexpr u32 kRpcInitialisedRegister = 0;

// TODO: derive real values
constexpr u32 kCommandBuffer = 0x00019600;
constexpr u32 kServerBase = 0x00100000;
constexpr u32 kServerStride = 0x1000;
constexpr u32 kServerBufferOffset = 0x800;

// TODO: derive real values. Must outlast SifIopReset @ 0x50c848 clearing SIFINIT and CMDINIT on
// the two instructions after the transfer.
constexpr u64 kRebootTicks = 64;

}  // namespace

Sif::Sif(Memory& memory, Sbus& sbus, std::string& console)
    : mMemory(memory), mSbus(sbus), mFileIo(memory, console), mLoadFile(memory),
      mCdvd(memory) {
    registerServer(mFileIo);
    registerServer(mLoadFile);
    registerServer(mCdvd);

    // A game starts with the IOP already booted by the BIOS.
    mSbus.writeSubToMainCommand(kCommandBuffer);
    mSbus.raiseSubToMainFlag(sbus::kSifInitialised | sbus::kCommandInitialised |
                             sbus::kBootFinished);
}

Sif::Transfer Sif::readTransfer(u32 address) const {
    return Transfer{mMemory.read32(address), mMemory.read32(address + 4),
                    mMemory.read32(address + 8), mMemory.read32(address + 12)};
}

// SIFMAN: sifman_SifSetDma @ 0x740
void Sif::receive(u32 transfers, u32 count, u64 hsync) {
    std::optional<Transfer> payload;
    for (u32 i = 0; i < count; ++i) {
        const Transfer transfer = readTransfer(transfers + i * kTransferSize);
        if ((transfer.attribute & kInterruptOnSend) == 0) {
            payload = transfer;
            continue;
        }
        handleCommand(transfer, payload, hsync);
        payload.reset();
    }
}

void Sif::handleCommand(const Transfer& command, const std::optional<Transfer>& payload,
                        u64 hsync) {
    const u32 packet = command.source;
    switch (mMemory.read32(packet + kHeaderCommand * 4)) {
        case sifcmd::kResetCommand:
            reset(hsync);
            return;
        case sifcmd::kInitCommand:
            initialiseCommand(packet);
            return;
        case sifcmd::kRpcBind:
            bindServer(packet);
            return;
        case sifcmd::kRpcCall:
            callServer(packet, payload);
            return;
        case sifcmd::kChangeSubAddress:
            return;
        default:
            reportStub(std::format("sif command {:08x}",
                                   mMemory.read32(packet + kHeaderCommand * 4)));
            return;
    }
}

// SIFCMD, in SLUS-216.21: sceSifResetIop @ 0x50c848 waits out. It clears the
// flags after this returns, so finishing here would lose them.
void Sif::reset(u64 hsync) {
    mRebootAt = hsync + kRebootTicks;
    mRebooting = true;
    mEeCommandBuffer = 0;
    mBound.clear();
    mPending.clear();
}

// SIFCMD: sifcmd_SifInitCmd @ 0x2c8
void Sif::initialiseCommand(u32 packet) {
    if (mMemory.read32(packet + kHeaderOption * 4) == 0) {
        mEeCommandBuffer = mMemory.read32(packet + kInitBuffer * 4);
        return;
    }
    std::vector<u32> reply(kRegisterWords, 0);
    reply[kHeaderSize] = kRegisterWords * 4;
    reply[kHeaderCommand] = sifcmd::kSetRegister;
    reply[kRegisterIndex] = kRpcInitialisedRegister;
    reply[kRegisterValue] = 1;
    send(std::move(reply));
}

// SIFCMD: sifcmd_SifRegisterRpc @ 0x1130
void Sif::registerServer(IopHleServer& server) {
    for (const u32 serverId : server.serverIds()) {
        const auto held = mRegistered.find(serverId);
        if (held != mRegistered.end() && held->second != &server) {
            reportStub(std::format("{} takes {:08x} from {}", server.name(), serverId,
                                   held->second->name()));
        }
        mRegistered[serverId] = &server;
    }
}

IopHleServer* Sif::serverFor(u32 serverId) const {
    const auto found = mRegistered.find(serverId);
    return found == mRegistered.end() ? nullptr : found->second;
}

u32 Sif::serverAddress(u32 serverId) {
    for (usize i = 0; i < mBound.size(); ++i) {
        if (mBound[i] == serverId) {
            return kServerBase + static_cast<u32>(i) * kServerStride;
        }
    }
    mBound.push_back(serverId);
    return kServerBase + static_cast<u32>(mBound.size() - 1) * kServerStride;
}

u32 Sif::serverIdAt(u32 address) const {
    if (address < kServerBase) {
        return 0;
    }
    const u32 index = (address - kServerBase) / kServerStride;
    return index < mBound.size() ? mBound[index] : 0;
}

// SIFCMD: sifcmd_SifBindRpc @ 0xd08
void Sif::bindServer(u32 packet) {
    const u32 serverId = mMemory.read32(packet + kBindServerId * 4);
    const u32 server = serverAddress(serverId);
    if (serverFor(serverId) == nullptr) {
        reportStub(std::format("sif server {:08x}", serverId));
    }
    replyToRequest(packet, sifcmd::kRpcBind, server, server + kServerBufferOffset);
}

void Sif::callServer(u32 packet, const std::optional<Transfer>& payload) {
    const u32 serverId = serverIdAt(mMemory.read32(packet + kCallServer * 4));
    const u32 number = mMemory.read32(packet + kCallNumber * 4);
    const u32 receive = mMemory.read32(packet + kCallReceiveBuffer * 4);
    const u32 receiveSize = mMemory.read32(packet + kCallReceiveSize * 4);
    const RpcCall call{number, payload ? payload->source : 0, receive, receiveSize};

    IopHleServer* server = serverFor(serverId);
    if (server == nullptr) {
        reportStub(std::format("sif server {:08x} call {}", serverId, number));
    }
    const s32 result = server == nullptr ? -1 : server->call(serverId, call);
    if (receiveSize >= 4) {
        mMemory.write32(receive, static_cast<u32>(result));
    }
    replyToRequest(packet, sifcmd::kRpcCall, 0, 0);
}

// SIFCMD: the SifRpcRendPkt_t sifcmd_SifCallRpc @ 0xed4 waits for.
void Sif::replyToRequest(u32 packet, u32 request, u32 server, u32 buffer) {
    std::vector<u32> reply(kReplyWords, 0);
    reply[kHeaderSize] = kReplyWords * 4;
    reply[kHeaderCommand] = sifcmd::kRpcEnd;
    reply[kRequestRecordId] = mMemory.read32(packet + kRequestRecordId * 4);
    reply[kRequestPacket] = mMemory.read32(packet + kRequestPacket * 4);
    reply[kRequestRpcId] = mMemory.read32(packet + kRequestRpcId * 4);
    reply[kRequestClient] = mMemory.read32(packet + kRequestClient * 4);
    reply[kReplyCommand] = request;
    reply[kReplyServer] = server;
    reply[kReplyBuffer] = buffer;
    send(std::move(reply));
}

void Sif::send(std::vector<u32> packet) {
    mPending.push_back(std::move(packet));
}

bool Sif::advance(u64 hsync) {
    if (mRebooting && hsync >= mRebootAt) {
        mRebooting = false;
        mSbus.writeSubToMainCommand(kCommandBuffer);
        mSbus.raiseSubToMainFlag(sbus::kSifInitialised | sbus::kCommandInitialised |
                                 sbus::kBootFinished);
    }
    if (mPending.empty() || mEeCommandBuffer == 0) {
        return false;
    }
    // SLUS-216.21: sceSifCmdIntrHdlr @ 0x5066b0 clears this word once it takes a packet.
    if (mMemory.read32(mEeCommandBuffer) != 0) {
        return false;
    }
    const std::vector<u32>& packet = mPending.front();
    for (usize i = 0; i < packet.size(); ++i) {
        mMemory.write32(mEeCommandBuffer + static_cast<u32>(i) * 4, packet[i]);
    }
    mPending.pop_front();
    return true;
}

}  // namespace uee
