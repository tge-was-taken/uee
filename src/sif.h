#pragma once

#include "iop.h"
#include "iop_hle_cdvdfsv.h"
#include "iop_hle_fileio.h"
#include "iop_hle_loadfile.h"
#include "iop_rpc.h"
#include "memory.h"
#include "sbus.h"
#include "sif_server.h"
#include "types.h"

#include <deque>
#include <filesystem>
#include <map>
#include <set>
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
constexpr u32 kSetDmaMarker = 0x80000004;
constexpr u32 kRpcEnd = 0x80000008;
constexpr u32 kRpcBind = 0x80000009;
constexpr u32 kRpcCall = 0x8000000A;
constexpr u32 kRpcReadData = 0x8000000C;

}  // namespace sifcmd

// The EE's half of the SIF conversation, carried without an IOP. Answers go
// back as command packets so the guest's own SIF library parses them.
class Sif {
public:
    Sif(Memory& memory, Sbus& sbus, std::string& console, Iop& iop);

    void attachDisc(Disc& disc) {
        mDisc = &disc;
        mCdvd.attachDisc(disc);
        mLoadFile.attachDisc(disc);
    }

    void attachFiles(std::filesystem::path root) { mFileIo.attachFiles(std::move(root)); }

    // A later registration takes the service ids off the one that held them,
    // which is what a game shipping its own build of a module does.
    void registerServer(SifServer& server);

    // What the reply queue looks like, for when the EE stops making progress.
    void report() const;

    void receive(u32 transfers, u32 count);

    // True when a packet landed and the SIF0 interrupt is owed.
    bool advance();

private:
    struct Transfer {
        u32 source = 0;
        u32 destination = 0;
        u32 size = 0;
        u32 attribute = 0;
    };

    Transfer readTransfer(u32 address) const;
    void sendToIop(const Transfer& transfer);
    void handleCommand(const Transfer& command, const std::optional<Transfer>& payload);
    void reset(u32 packet);
    void loadRebootImage(const std::string& argument);
    void initialiseCommand(u32 packet);
    void bindServer(u32 packet);
    void callServer(u32 packet, const std::optional<Transfer>& payload);
    void readOtherData(u32 packet);
    void replyToRequest(u32 packet, u32 request, u32 server, u32 buffer);
    void send(std::vector<u32> packet);
    // Deduplicate stub logs to reduce spam
    void reportOnce(u32 serverId, u32 number);

    SifServer* serverFor(u32 serverId) const;
    u32 serverAddress(u32 serverId);
    u32 serverIdAt(u32 address) const;

    Memory& mMemory;
    Sbus& mSbus;
    Iop& mIop;
    IopHleFileio mFileIo;
    IopHleLoadfile mLoadFile;
    IopHleCdvdfsv mCdvd;
    IopRpc mIopRpc;
    std::map<u32, SifServer*> mRegistered;
    u32 mUndelivered = 0;

    u32 mEeCommandBuffer = 0;
    bool mRebooting = false;
    Disc* mDisc = nullptr;
    std::map<u32, u32> mBound;
    std::set<std::pair<u32, u32>> mReported;
    std::deque<std::vector<u32>> mPending;
};

}  // namespace uee
