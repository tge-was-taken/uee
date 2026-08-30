#include "iop_rpc.h"

#include "log.h"

namespace uee {
namespace {

LoggerChannel log("ioprpc");

}  // namespace

IopRpc::IopRpc(Memory& memory, Iop& iop) : mMemory(memory), mIop(iop) {}

std::span<const u32> IopRpc::serverIds() const {
    mIds.clear();
    for (const IopRpcServer& server : mIop.rpcServers()) {
        mIds.push_back(server.id);
    }
    return mIds;
}

const IopRpcServer* IopRpc::serverFor(u32 serverId) const {
    for (const IopRpcServer& server : mIop.rpcServers()) {
        if (server.id == serverId) {
            return &server;
        }
    }
    return nullptr;
}

u32 IopRpc::bufferOf(u32 serverId) const {
    const IopRpcServer* server = serverFor(serverId);
    return server == nullptr ? 0 : server->buffer;
}

u32 IopRpc::dataOf(u32 serverId) const {
    const IopRpcServer* server = serverFor(serverId);
    return server == nullptr ? 0 : server->data;
}

// SIFCMD: sceSifExecRequest @ 0x13a4 hands the handler the request number, the
// buffer the caller wrote into and its size, and sends back what it returns.
s32 IopRpc::call(u32 serverId, const RpcCall& call) {
    const IopRpcServer* server = serverFor(serverId);
    if (server == nullptr) {
        return -1;
    }

    Memory& iopMemory = mIop.memory();
    for (u32 i = 0; i < call.argumentSize; ++i) {
        iopMemory.write8(call.destination + i, mMemory.read8(call.argument + i));
    }

    log.trace("server {:08x} call {:x}, {} bytes in, {} out", serverId, call.number,
              call.argumentSize, call.receiveSize);
    const u32 arguments[3] = {call.number, server->buffer, call.argumentSize};
    const u32 reply = mIop.callFunction(server->handler, server->gp, arguments);

    // A handler that refuses a request answers with something that is not an
    // address, and reading from it would fault the machine on its behalf.
    if (reply >= kIopRamSize) {
        log.warn("server {:08x} call {:x} answered {:08x}, which is not a reply", serverId,
                 call.number, reply);
        return 0;
    }
    if (call.receiveSize < 4) {
        return 0;
    }
    for (u32 i = 0; i < call.receiveSize; ++i) {
        mMemory.write8(call.receive + i, iopMemory.read8(reply + i));
    }
    // Sif writes this back over the first word, so it has to be what the
    // handler already put there.
    return static_cast<s32>(mMemory.read32(call.receive));
}

}  // namespace uee
