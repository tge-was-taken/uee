#pragma once

#include "memory.h"
#include "types.h"

namespace uee {

enum class SifCommand {
    Bind,
    Call,
    Other,
};

struct SifRequest {
    SifCommand command = SifCommand::Other;
    u32 packet = 0;
    u32 clientData = 0;
    s32 semaphore = -1;
    u32 endFunction = 0;
    u32 endParameter = 0;
    s32 serverId = 0;
    s32 rpcNumber = 0;
    u32 sendBuffer = 0;
    u32 sendSize = 0;
    u32 receiveBuffer = 0;
    u32 receiveSize = 0;
};

SifRequest decodeSifDma(Memory& memory, u32 transfers, u32 count);
void writeBindReply(Memory& memory, u32 clientData, u32 serverBuffer);
void releaseRequest(Memory& memory, const SifRequest& request);

}  // namespace uee
