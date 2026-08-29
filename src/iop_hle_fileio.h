#pragma once

#include "iop.h"
#include "memory.h"
#include "sif_server.h"
#include "types.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>

namespace uee {

class IopHleFileio final : public SifServer {
public:
    IopHleFileio(Memory& memory, std::string& console, Iop& iop);

    // Guest paths resolve under here whatever device they name, so a game that
    // expects its data on a memory card or a USB disc reads it from one place.
    void attachFiles(std::filesystem::path root) { mRoot = std::move(root); }

    std::string_view name() const override { return "FILEIO"; }
    std::span<const u32> serverIds() const override;
    s32 call(u32 serverId, const RpcCall& call) override;

private:
    s32 open(u32 argument);
    s32 close(u32 argument);
    s32 read(u32 argument);
    s32 seek(u32 argument);
    s32 write(u32 argument);

    std::filesystem::path resolve(std::string_view path) const;
    std::fstream* fileFor(s32 descriptor);

    Memory& mMemory;
    std::string& mConsole;
    Iop& mIop;
    std::filesystem::path mRoot;
    std::map<s32, std::fstream> mOpen;
    // Below this are the three the console holds.
    s32 mNextDescriptor = 3;
};

}  // namespace uee
