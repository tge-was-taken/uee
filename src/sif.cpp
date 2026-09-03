// SIFCMD md5 bcdaf8bead75a7362a82c95bbbf479c7, SIFMAN md5
// 35a6ac86ca2d7ee9c5a27de8328c3267, SLUS_216.21 md5 9074ba25f5a100940ed233c9f8f675ab.

#include "disc.h"
#include "log.h"

#include <algorithm>
#include <cstring>
#include "sif.h"


#include <format>
#include <utility>

namespace uee {
namespace {

LoggerChannel log("sif");

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

constexpr u32 kReadSource = 8;
constexpr u32 kReadDestination = 9;
constexpr u32 kReadSize = 10;

constexpr u32 kReplyCommand = 8;
constexpr u32 kReplyServer = 9;
constexpr u32 kReplyBuffer = 10;
constexpr u32 kReplyWords = 16;

constexpr u32 kRegisterWords = 6;
constexpr u32 kRegisterIndex = 4;
constexpr u32 kRegisterValue = 5;
constexpr u32 kRpcInitialisedRegister = 0;

// A reply the EE never collects stops every reply behind it.
constexpr u32 kUndeliveredReported = 20000;
// SIFCMD: the reset packet's argument follows its length and mode.
constexpr u32 kResetArgumentLength = 4;
constexpr u32 kResetArgument = 24;
constexpr u32 kResetArgumentLimit = 80;

// A ROMDIR entry names a file, its extra info and its length.
constexpr usize kRomdirEntryBytes = 16;
constexpr usize kRomdirNameBytes = 10;
constexpr usize kRomdirSize = 12;
constexpr usize kRomdirAlignment = 16;

bool isIrx(std::span<const u8> image) {
    return image.size() > 4 && image[0] == 0x7F && image[1] == 'E' && image[2] == 'L' &&
           image[3] == 'F';
}

constexpr u32 kCommandBuffer = 0x00019600;
constexpr u32 kServerBase = 0x01000000;
constexpr u32 kServerStride = 0x1000;
constexpr u32 kServerBufferOffset = 0x800;

constexpr u32 kBootFlags =
    sbus::kSifInitialised | sbus::kCommandInitialised | sbus::kBootFinished;

}  // namespace

Sif::Sif(Memory& memory, Sbus& sbus, std::string& console, Iop& iop)
    : mMemory(memory), mSbus(sbus), mIop(iop), mFileIo(memory, console, iop),
      mLoadFile(memory, iop), mCdvd(memory), mIopRpc(memory, iop) {
    registerServer(mFileIo);
    registerServer(mLoadFile);
    registerServer(mCdvd);

    // A game starts with the IOP already booted by the BIOS.
    mSbus.writeSubToMainCommand(kCommandBuffer);
    mSbus.raiseSubToMainFlag(kBootFlags);
}

Sif::Transfer Sif::readTransfer(u32 address) const {
    return Transfer{mMemory.read32(address), mMemory.read32(address + 4),
                    mMemory.read32(address + 8), mMemory.read32(address + 12)};
}

// SIFMAN: sifman_SifSetDma @ 0x740
// Without the interrupt attribute a transfer carries data, not a command.
void Sif::sendToIop(const Transfer& transfer) {
    if (transfer.destination >= kIopRamSize || transfer.size == 0) {
        return;
    }
    log.trace("dma {:08x} to {:08x}, {} bytes", transfer.source, transfer.destination,
              transfer.size);
    Memory& iopMemory = mIop.memory();
    for (u32 i = 0; i < transfer.size; ++i) {
        iopMemory.write8(transfer.destination + i, mMemory.read8(transfer.source + i));
    }
}

void Sif::receive(u32 transfers, u32 count) {
    std::optional<Transfer> payload;
    for (u32 i = 0; i < count; ++i) {
        const Transfer transfer = readTransfer(transfers + i * kTransferSize);
        if ((transfer.attribute & kInterruptOnSend) == 0) {
            payload = transfer;
            sendToIop(transfer);
            continue;
        }
        handleCommand(transfer, payload);
        payload.reset();
    }
}

void Sif::handleCommand(const Transfer& command, const std::optional<Transfer>& payload) {
    const u32 packet = command.source;
    switch (mMemory.read32(packet + kHeaderCommand * 4)) {
        case sifcmd::kResetCommand:
            reset(packet);
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
        case sifcmd::kRpcReadData:
            readOtherData(packet);
            return;
        case sifcmd::kChangeSubAddress:
            return;
        // Unhandled by design: SifCmdIntrHandler @ 0x590 finds no entry for
        // it. sceSifSetDmaInternal @ 0x505fc0 sends it to raise the interrupt.
        case sifcmd::kSetDmaMarker:
            return;
        default:
            log.stub("command {:08x}",
                                   mMemory.read32(packet + kHeaderCommand * 4));
            return;
    }
}

// SIFCMD: the reset packet carries the argument sceSifResetIop was given, which
// names the module set the IOP is to come back up with.
void Sif::reset(u32 packet) {
    mRebooting = true;
    mEeCommandBuffer = 0;
    mBound.clear();
    mPending.clear();

    const u32 length = std::min(mMemory.read32(packet + kResetArgumentLength * 4), kResetArgumentLimit);
    std::string argument;
    for (u32 i = 0; i < length; ++i) {
        const char letter = static_cast<char>(mMemory.read8(packet + kResetArgument + i));
        if (letter == '\0') {
            break;
        }
        argument.push_back(letter);
    }
    log.trace("sceSifResetIop({})", argument);
    loadRebootImage(argument);
}

// A ROMDIR archive: fixed size entries, then each file padded to alignment.
void Sif::loadRebootImage(const std::string& argument) {
    const usize named = argument.rfind("cdrom0:");
    if (mDisc == nullptr || named == std::string::npos) {
        return;
    }
    std::string path = argument.substr(named + std::strlen("cdrom0:"));
    path.resize(std::min(path.find(';'), path.size()));
    const std::optional<DiscFile> file = mDisc->find(path);
    if (!file) {
        log.stub("the reboot image {} is not on the disc", path);
        return;
    }

    std::vector<u8> image;
    const u32 sectors = (file->size + kSectorBytes - 1) / kSectorBytes;
    if (!mDisc->read(file->sector, sectors, image)) {
        log.stub("the reboot image {} could not be read", path);
        return;
    }

    usize data = 0;
    usize loaded = 0;
    for (usize entry = 0; entry + kRomdirEntryBytes <= image.size(); entry += kRomdirEntryBytes) {
        std::string name(reinterpret_cast<const char*>(&image[entry]), kRomdirNameBytes);
        name.resize(std::min(name.find('\0'), name.size()));
        if (name.empty()) {
            break;
        }
        const u32 size = static_cast<u32>(image[entry + kRomdirSize]) |
                         static_cast<u32>(image[entry + kRomdirSize + 1]) << 8 |
                         static_cast<u32>(image[entry + kRomdirSize + 2]) << 16 |
                         static_cast<u32>(image[entry + kRomdirSize + 3]) << 24;
        const usize aligned = (size + kRomdirAlignment - 1) & ~(kRomdirAlignment - 1);
        if (data + size <= image.size() && isIrx(std::span(image).subspan(data, size))) {
            mIop.loadModule(name, std::span(image).subspan(data, size));
            ++loaded;
        }
        data += aligned;
    }
    log.info("{} carries {} modules", path, loaded);
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
void Sif::registerServer(SifServer& server) {
    for (const u32 serverId : server.serverIds()) {
        const auto held = mRegistered.find(serverId);
        if (held != mRegistered.end() && held->second != &server) {
            log.stub("{} takes {:08x} from {}", server.name(), serverId,
                                   held->second->name());
        }
        mRegistered[serverId] = &server;
    }
}

void Sif::reportOnce(u32 serverId, u32 number) {
    if (mReported.emplace(serverId, number).second) {
        log.stub("server {:08x} call {}", serverId, number);
    }
}

SifServer* Sif::serverFor(u32 serverId) const {
    const auto found = mRegistered.find(serverId);
    return found == mRegistered.end() ? nullptr : found->second;
}

u32 Sif::serverAddress(u32 serverId) {
    for (const auto& [address, bound] : mBound) {
        if (bound == serverId) {
            return address;
        }
    }
    const u32 registered = mIopRpc.dataOf(serverId);
    const u32 address =
        registered != 0 ? registered
                        : kServerBase + static_cast<u32>(mBound.size()) * kServerStride;
    mBound[address] = serverId;
    return address;
}

u32 Sif::serverIdAt(u32 address) const {
    const auto found = mBound.find(address);
    return found == mBound.end() ? 0 : found->second;
}

// SIFCMD: sifcmd_SifBindRpc @ 0xd08
void Sif::bindServer(u32 packet) {
    const u32 serverId = mMemory.read32(packet + kBindServerId * 4);
    log.trace("SifCmdRpcBind(server={:08x})", serverId);
    registerServer(mIopRpc);
    const u32 server = serverAddress(serverId);
    if (serverFor(serverId) == nullptr) {
        log.stub("server {:08x}", serverId);
    }
    const u32 registered = mIopRpc.bufferOf(serverId);
    replyToRequest(packet, sifcmd::kRpcBind, server,
                   registered != 0 ? registered : server + kServerBufferOffset);
}

void Sif::callServer(u32 packet, const std::optional<Transfer>& payload) {
    const u32 serverId = serverIdAt(mMemory.read32(packet + kCallServer * 4));
    const u32 number = mMemory.read32(packet + kCallNumber * 4);
    const u32 receive = mMemory.read32(packet + kCallReceiveBuffer * 4);
    const u32 receiveSize = mMemory.read32(packet + kCallReceiveSize * 4);
    const RpcCall call{number,
                       payload ? payload->source : 0,
                       payload ? payload->size : 0,
                       payload ? payload->destination : 0,
                       receive,
                       receiveSize};
    log.trace("SifExecRequest(server={:08x}, number={}, send={} bytes, receive={} bytes)",
              serverId, number, call.argumentSize, receiveSize);

    SifServer* server = serverFor(serverId);
    if (server == nullptr) {
        reportOnce(serverId, number);
    }
    const s32 result = server == nullptr ? -1 : server->call(serverId, call);
    log.trace("SifExecRequest({:08x}, {}) answered {}", serverId, number, result);
    if (receiveSize >= 4) {
        mMemory.write32(receive, static_cast<u32>(result));
    }
    replyToRequest(packet, sifcmd::kRpcCall, 0, 0);
}

// SIFCMD: the 0x8000000C handler @ 0xa68
void Sif::readOtherData(u32 packet) {
    const u32 source = mMemory.read32(packet + kReadSource * 4);
    const u32 destination = mMemory.read32(packet + kReadDestination * 4);
    const u32 size = mMemory.read32(packet + kReadSize * 4);
    log.trace("SifGetOtherData(iop={:08x}, ee={:08x}, {} bytes)", source, destination, size);
    for (u32 i = 0; i < size; ++i) {
        mMemory.write8(destination + i, mIop.memory().read8(source + i));
    }

    std::vector<u32> reply(kReplyWords, 0);
    reply[kHeaderSize] = kReplyWords * 4;
    reply[kHeaderCommand] = sifcmd::kRpcEnd;
    reply[kRequestPacket] = mMemory.read32(packet + kRequestPacket * 4);
    reply[kRequestClient] = mMemory.read32(packet + kRequestClient * 4);
    reply[kReplyCommand] = sifcmd::kRpcReadData;
    send(std::move(reply));
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

void Sif::report() const {
    log.info("{} replies queued, buffer {:08x} holds {:08x}", mPending.size(), mEeCommandBuffer,
             mEeCommandBuffer == 0 ? 0 : mMemory.read32(mEeCommandBuffer));
}

bool Sif::advance() {
    // SLUS-216.21: sceSifResetIop @ 0x50c848
    // It clears BOOTEND, SIFINIT and CMDINIT across three instructions, so
    // reporting the reboot earlier loses the ones not yet cleared.
    if (mRebooting && (mSbus.subToMainFlag() & kBootFlags) == 0) {
        mRebooting = false;
        mSbus.writeSubToMainCommand(kCommandBuffer);
        mSbus.raiseSubToMainFlag(kBootFlags);
    }
    if (mPending.empty()) {
        return false;
    }
    if (log.tracing() && mPending.size() > 2) {
        log.trace("{} replies waiting, buffer {:08x} holds {:08x}", mPending.size(),
                  mEeCommandBuffer,
                  mEeCommandBuffer == 0 ? 0 : mMemory.read32(mEeCommandBuffer));
    }
    if (mEeCommandBuffer == 0) {
        return false;
    }
    // SLUS-216.21: sceSifCmdIntrHdlr @ 0x5066b0 clears this word once it takes a packet.
    if (mMemory.read32(mEeCommandBuffer) != 0) {
        if (++mUndelivered == kUndeliveredReported) {
            log.warn("{} replies waiting on a packet the EE has not taken from {:08x}",
                     mPending.size(), mEeCommandBuffer);
        }
        return false;
    }
    mUndelivered = 0;
    const std::vector<u32>& packet = mPending.front();
    for (usize i = 0; i < packet.size(); ++i) {
        mMemory.write32(mEeCommandBuffer + static_cast<u32>(i) * 4, packet[i]);
    }
    mPending.pop_front();
    return true;
}

}  // namespace uee
