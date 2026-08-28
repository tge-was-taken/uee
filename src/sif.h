#pragma once

#include "iop_cdvdfsv.h"
#include "iop_fileio.h"
#include "iop_loadfile.h"
#include "iop_server.h"
#include "iop_cdvdfsv.h"
#include "iop_fileio.h"
#include "iop_loadfile.h"
#include "iop_server.h"
#include "memory.h"
#include "sbus.h"
#include "types.h"

#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace uee {

class Disc;

namespace sifcmd {

constexpr u32 kChangeSubAddress = 0x80000000;
constexpr u32 kSetRegister = 0x80000001;
constexpr u32 kInitCommand = 0x80000002;
constexpr u32 kResetCommand = 0x80000003;
constexpr u32 kRpcEnd = 0x80000008;
constexpr u32 kRpcBind = 0x80000009;
constexpr u32 kRpcCall = 0x8000000A;

}  // namespace sifcmd

// The EE's half of the SIF conversation, carried without an IOP. Answers go
// back as command packets so the guest's own SIF library parses them.
class Sif {
public:
    Sif(Memory& memory, Sbus& sbus, std::string& console);

    void attachDisc(Disc& disc) { mCdvd.attachDisc(disc); }

    // A later registration takes the service ids off the one that held them,
    // which is what a game shipping its own build of a module does.
    void registerServer(IopServer& server);

    void receive(u32 transfers, u32 count, u64 hsync);

    // True when a packet landed and the SIF0 interrupt is owed.
    bool advance(u64 hsync);

private:
    struct Transfer {
        u32 source = 0;
        u32 destination = 0;
        u32 size = 0;
        u32 attribute = 0;
    };

    Transfer readTransfer(u32 address) const;
    void handleCommand(const Transfer& command, const std::optional<Transfer>& payload,
                       u64 hsync);
    void reset(u64 hsync);
    void initialiseCommand(u32 packet);
    void bindServer(u32 packet);
    void callServer(u32 packet, const std::optional<Transfer>& payload);
    void replyToRequest(u32 packet, u32 request, u32 server, u32 buffer);
    void send(std::vector<u32> packet);

    IopServer* serverFor(u32 serverId) const;
    u32 serverAddress(u32 serverId);
    u32 serverIdAt(u32 address) const;

    Memory& mMemory;
    Sbus& mSbus;
    IopFileio mFileIo;
    IopLoadfile mLoadFile;
    IopCdvdfsv mCdvd;
    std::map<u32, IopServer*> mRegistered;

    u32 mEeCommandBuffer = 0;
    u64 mRebootAt = 0;
    bool mRebooting = false;
    std::vector<u32> mBound;
    std::deque<std::vector<u32>> mPending;
};

}  // namespace uee
