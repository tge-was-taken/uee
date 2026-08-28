#pragma once

#include "types.h"

#include <span>
#include <string_view>

namespace uee {

// SIFCMD: the arguments sifcmd_SifExecRequest @ 0x13a4 hands a server.
struct RpcCall {
    u32 number = 0;
    u32 argument = 0;
    u32 receive = 0;
    u32 receiveSize = 0;
};

// One IOP module's RPC servers. Modules register their ids with SifRegisterRpc
// as they start, so serverIds may grow and Sif::registerServer may be called
// again: an emulated IOP claims ids as its guest modules register them, and a
// game shipping its own build of a module claims ids another one held.
class IopServer {
public:
    virtual ~IopServer() = default;

    virtual std::string_view name() const = 0;
    virtual std::span<const u32> serverIds() const = 0;
    virtual s32 call(u32 serverId, const RpcCall& call) = 0;
};

}  // namespace uee
