#pragma once

#include "iop_hle_server.h"
#include "memory.h"
#include "types.h"

#include <string>

namespace uee {

class IopHleFileio final : public IopHleServer {
public:
    IopHleFileio(Memory& memory, std::string& console);

    std::string_view name() const override { return "FILEIO"; }
    std::span<const u32> serverIds() const override;
    s32 call(u32 serverId, const RpcCall& call) override;

private:
    s32 write(u32 argument);
    u32 allocate(u32 size);

    Memory& mMemory;
    std::string& mConsole;
    u32 mHeapEnd;
};

}  // namespace uee
