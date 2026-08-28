#pragma once

#include "iop_hle_server.h"
#include "memory.h"
#include "types.h"

namespace uee {

class Disc;

class IopHleCdvdfsv final : public IopHleServer {
public:
    explicit IopHleCdvdfsv(Memory& memory);

    void attachDisc(Disc& disc) { mDisc = &disc; }

    std::string_view name() const override { return "CDVDFSV"; }
    std::span<const u32> serverIds() const override;
    s32 call(u32 serverId, const RpcCall& call) override;

private:
    s32 read(u32 argument);
    s32 search(u32 argument);

    Memory& mMemory;
    Disc* mDisc = nullptr;
};

}  // namespace uee
