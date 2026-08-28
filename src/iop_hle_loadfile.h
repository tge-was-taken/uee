#pragma once

#include "iop_hle_server.h"
#include "memory.h"
#include "types.h"

namespace uee {

class IopHleLoadfile final : public IopHleServer {
public:
    explicit IopHleLoadfile(Memory& memory);

    std::string_view name() const override { return "LOADFILE"; }
    std::span<const u32> serverIds() const override;
    s32 call(u32 serverId, const RpcCall& call) override;

private:
    s32 loadModule(const RpcCall& call);

    Memory& mMemory;
    u32 mModules = 0;
};

}  // namespace uee
