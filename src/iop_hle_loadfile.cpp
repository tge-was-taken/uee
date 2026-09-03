// LOADFILE md5 12335f3c69c0b335624dd0328bd8bee4.

#include "log.h"
#include "iop_hle_loadfile.h"

#include "disc.h"

#include <format>
#include <string>
#include <vector>

namespace uee {
namespace {

LoggerChannel log("loadfile");

// LOADFILE: LoadFileRpcServerThread @ 0xc8
constexpr u32 kLoadFile = 0x80000006;
constexpr u32 kServerIds[] = {kLoadFile};

// LOADFILE: RpcServeLoop @ 0x4c4, table at 0x1bc8
constexpr u32 kLoadModule = 0;
constexpr u32 kLoadModuleBuffer = 6;

// SLUS-216.21: lf_version @ 0x50bbe8
// It refuses anything but "3100", the version of the IOPRP310.IMG the game
// replaces its IOP modules from.
constexpr u32 kVersion = 255;
constexpr u32 kVersion3100 = 0x30303133;

// SLUS-216.21: sceSifLoadModule @ 0x50c2e8
constexpr u32 kPath = 8;
constexpr u32 kPathLimit = 252;
constexpr u32 kEntryResult = 1;

}  // namespace

IopHleLoadfile::IopHleLoadfile(Memory& memory, Iop& iop) : mMemory(memory), mIop(iop) {}

std::span<const u32> IopHleLoadfile::serverIds() const { return kServerIds; }

s32 IopHleLoadfile::call(u32 serverId, const RpcCall& call) {
    if (call.number == kVersion) {
        return static_cast<s32>(kVersion3100);
    }
    if (call.number == kLoadModule) {
        return loadModule(call);
    }
    if (call.number == kLoadModuleBuffer) {
        return loadModuleBuffer(call);
    }
    log.stub("{:08x} call {}", serverId, call.number);
    return -1;
}

std::string IopHleLoadfile::readPath(u32 address) {
    std::string path;
    for (u32 i = 0; i < kPathLimit; ++i) {
        const char letter = static_cast<char>(mMemory.read8(address + i));
        if (letter == '\0') {
            break;
        }
        path.push_back(letter);
    }
    return path;
}

// LOADFILE: LoadModuleFromFile @ 0x10dc
s32 IopHleLoadfile::loadModule(const RpcCall& call) {
    const std::string path = readPath(call.argument + kPath);
    log.trace("LoadModuleFromFile(path={})", path);
    if (call.receiveSize >= 8) {
        mMemory.write32(call.receive + kEntryResult * 4, 0);
    }

    std::optional<DiscFile> file;
    if (mDisc != nullptr) {
        file = mDisc->find(path);
    }
    if (!file) {
        log.stub("module {}", path);
        return static_cast<s32>(++mModules);
    }

    std::vector<u8> image;
    const u32 sectors = (file->size + kSectorBytes - 1) / kSectorBytes;
    const std::string module = file->name.substr(0, file->name.find(';'));
    if (!mDisc->read(file->sector, sectors, image) || !mIop.loadModule(module, image)) {
        log.stub("module {}", path);
        return static_cast<s32>(++mModules);
    }
    return static_cast<s32>(++mModules);
}

s32 IopHleLoadfile::loadModuleBuffer(const RpcCall& call) {
    constexpr u32 kBuffer = 0;
    log.stub("module buffer at {:08x}", mMemory.read32(call.argument + kBuffer * 4));
    if (call.receiveSize >= 8) {
        mMemory.write32(call.receive + kEntryResult * 4, 0);
    }
    return static_cast<s32>(++mModules);
}

}  // namespace uee
