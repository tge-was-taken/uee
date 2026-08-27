#include "sif.h"

namespace uee {
namespace {

constexpr u32 kTransferSize = 16;
constexpr u32 kTransferSource = 0;
constexpr u32 kTransferLength = 8;

constexpr u32 kHeaderCommand = 8;

constexpr u32 kPacketRecordId = 16;
constexpr u32 kPacketAllocated = 0x01;

constexpr u32 kPacketClientData = 28;
constexpr u32 kBindServerId = 32;
constexpr u32 kCallRpcNumber = 32;
constexpr u32 kCallSendSize = 36;
constexpr u32 kCallReceiveBuffer = 40;
constexpr u32 kCallReceiveSize = 44;

constexpr u32 kClientPacket = 0;
constexpr u32 kClientSemaphore = 8;
constexpr u32 kClientBuffer = 20;
constexpr u32 kClientEndFunction = 28;
constexpr u32 kClientEndParameter = 32;
constexpr u32 kClientServer = 36;

constexpr u32 kCommandBind = 0x80000009;
constexpr u32 kCommandCall = 0x8000000A;

}  // namespace

SifRequest decodeSifDma(Memory& memory, u32 transfers, u32 count) {
    SifRequest request;
    if (count == 0) {
        return request;
    }

    // _SifSendCmd always puts the command packet last and any payload before it.
    const u32 last = transfers + (count - 1) * kTransferSize;
    const u32 packet = memory.read32(last + kTransferSource);

    switch (memory.read32(packet + kHeaderCommand)) {
        case kCommandBind: request.command = SifCommand::Bind; break;
        case kCommandCall: request.command = SifCommand::Call; break;
        default: return request;
    }

    request.packet = packet;
    request.clientData = memory.read32(packet + kPacketClientData);
    request.semaphore = static_cast<s32>(memory.read32(request.clientData + kClientSemaphore));
    request.endFunction = memory.read32(request.clientData + kClientEndFunction);
    request.endParameter = memory.read32(request.clientData + kClientEndParameter);

    if (request.command == SifCommand::Bind) {
        request.serverId = static_cast<s32>(memory.read32(packet + kBindServerId));
        return request;
    }

    request.rpcNumber = static_cast<s32>(memory.read32(packet + kCallRpcNumber));
    request.sendSize = memory.read32(packet + kCallSendSize);
    request.receiveBuffer = memory.read32(packet + kCallReceiveBuffer);
    request.receiveSize = memory.read32(packet + kCallReceiveSize);
    if (count > 1) {
        request.sendBuffer = memory.read32(transfers + kTransferSource);
        request.sendSize = memory.read32(transfers + kTransferLength);
    }
    return request;
}

void writeBindReply(Memory& memory, u32 clientData, u32 serverBuffer) {
    memory.write32(clientData + kClientServer, serverBuffer);
    memory.write32(clientData + kClientBuffer, serverBuffer);
}

// The guest allocates request packets from a fixed table and expects the reply
// to hand each one back, so skipping this exhausts the table after 32 calls.
void releaseRequest(Memory& memory, const SifRequest& request) {
    const u32 recordId = memory.read32(request.packet + kPacketRecordId);
    memory.write32(request.packet + kPacketRecordId, recordId & ~kPacketAllocated);
    memory.write32(request.clientData + kClientPacket, 0);
}

}  // namespace uee
