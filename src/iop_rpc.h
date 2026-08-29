#pragma once

#include "iop.h"
#include "memory.h"
#include "sif_server.h"
#include "types.h"

#include <span>
#include <string_view>
#include <vector>

namespace uee {

// The servers loaded IOP modules registered for themselves. A call runs the
// module's own handler rather than answering for it here.
class IopRpc final : public SifServer {
public:
    IopRpc(Memory& memory, Iop& iop);

    std::string_view name() const override { return "IOP"; }
    std::span<const u32> serverIds() const override;
    s32 call(u32 serverId, const RpcCall& call) override;

    // Zero when no module registered the server.
    u32 bufferOf(u32 serverId) const;
    u32 dataOf(u32 serverId) const;

private:
    const IopRpcServer* serverFor(u32 serverId) const;

    Memory& mMemory;
    Iop& mIop;
    mutable std::vector<u32> mIds;
};

}  // namespace uee
