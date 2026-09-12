#pragma once

#include "iop.h"
#include "memory.h"
#include "sif_server.h"
#include "types.h"

#include <string>
#include <vector>

namespace uee {

class Disc;

class IopHleLoadfile final : public SifServer {
public:
    IopHleLoadfile(Memory& memory, Iop& iop);

    void attachDisc(Disc& disc) { mDisc = &disc; }

    std::string_view name() const override { return "LOADFILE"; }
    std::span<const u32> serverIds() const override;
    s32 call(u32 serverId, const RpcCall& call) override;

private:
    s32 loadModule(const RpcCall& call);
    s32 loadModuleBuffer(const RpcCall& call);
    std::vector<std::string> readArguments(u32 address, u32 length);
    std::string readPath(u32 address);

    Memory& mMemory;
    Iop& mIop;
    Disc* mDisc = nullptr;
    u32 mModules = 0;
};

}  // namespace uee
