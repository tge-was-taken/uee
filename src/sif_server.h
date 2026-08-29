#pragma once

#include "types.h"

#include <span>
#include <string_view>

namespace uee {

// SIFCMD: the arguments sifcmd_SifExecRequest @ 0x13a4 hands a server.
struct RpcCall {
    u32 number = 0;
    u32 argument = 0;
    u32 argumentSize = 0;
    // Where the caller addressed the arguments on the IOP, which only a server
    // that runs there can do anything with.
    u32 destination = 0;
    u32 receive = 0;
    u32 receiveSize = 0;
};

// What answers an RPC call the EE makes over the SIF, on the emulated IOP or
// standing in for it. Modules register ids as they start, so serverIds grows.
class SifServer {
public:
    virtual ~SifServer() = default;

    virtual std::string_view name() const = 0;
    virtual std::span<const u32> serverIds() const = 0;
    virtual s32 call(u32 serverId, const RpcCall& call) = 0;
};

}  // namespace uee
