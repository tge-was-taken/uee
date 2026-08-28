// LOADFILE md5 12335f3c69c0b335624dd0328bd8bee4.

#include "iop_hle_loadfile.h"

#include "syscall_stubs.h"

#include <format>
#include <string>

namespace uee {
namespace {

// LOADFILE: LoadFileRpcServerThread @ 0xc8
constexpr u32 kLoadFile = 0x80000006;
constexpr u32 kServerIds[] = {kLoadFile};

// LOADFILE: RpcServeLoop @ 0x4c4, table at 0x1bc8
constexpr u32 kLoadModule = 0;

// That build answers 0 above the table, but SLUS-216.21: lf_version @ 0x50bbe8
// refuses anything but "3100": the game replaces the IOP modules at run time
// from cdrom0:\LIB31\IOPRP310.IMG, which carries version 3.10.
constexpr u32 kVersion = 255;
constexpr u32 kVersion3100 = 0x30303133;

// SLUS-216.21: sceSifLoadModule @ 0x50c2e8
constexpr u32 kPath = 8;
constexpr u32 kPathLimit = 252;
constexpr u32 kEntryResult = 1;

}  // namespace

IopHleLoadfile::IopHleLoadfile(Memory& memory) : mMemory(memory) {}

std::span<const u32> IopHleLoadfile::serverIds() const { return kServerIds; }

s32 IopHleLoadfile::call(u32 serverId, const RpcCall& call) {
    if (call.number == kVersion) {
        return static_cast<s32>(kVersion3100);
    }
    if (call.number == kLoadModule) {
        return loadModule(call);
    }
    reportStub(std::format("LOADFILE {:08x} call {}", serverId, call.number));
    return -1;
}

// LOADFILE: LoadModuleFromFile @ 0x10dc. Nothing here runs IOP code, so the
// module is answered for rather than loaded.
s32 IopHleLoadfile::loadModule(const RpcCall& call) {
    std::string path;
    for (u32 i = 0; i < kPathLimit; ++i) {
        const char letter = static_cast<char>(mMemory.read8(call.argument + kPath + i));
        if (letter == '\0') {
            break;
        }
        path.push_back(letter);
    }
    reportStub(std::format("module {}", path));

    if (call.receiveSize >= 8) {
        mMemory.write32(call.receive + kEntryResult * 4, 0);
    }
    return static_cast<s32>(++mModules);
}

}  // namespace uee
