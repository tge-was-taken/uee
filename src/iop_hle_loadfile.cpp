// LOADFILE md5 12335f3c69c0b335624dd0328bd8bee4.

#include "log.h"
#include "iop_hle_loadfile.h"

#include "disc.h"
#include "irx.h"

#include <algorithm>
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

// The arguments are strings laid end to end, each with its terminator.
std::vector<std::string> IopHleLoadfile::readArguments(u32 address, u32 length) {
    std::vector<std::string> arguments;
    std::string current;
    for (u32 i = 0; i < length; ++i) {
        const char letter = static_cast<char>(mMemory.read8(address + i));
        if (letter != '\0') {
            current.push_back(letter);
            continue;
        }
        arguments.push_back(current);
        current.clear();
    }
    if (!current.empty()) {
        arguments.push_back(current);
    }
    return arguments;
}

// ps2sdk loadfile.c: struct _lf_module_buffer_load_arg, the module's address on
// the IOP and the argument length, then the arguments behind an unused path.
s32 IopHleLoadfile::loadModuleBuffer(const RpcCall& call) {
    constexpr u32 kBuffer = 0;
    constexpr u32 kArgumentLength = 4;
    constexpr u32 kArguments = kPath + kPathLimit;
    constexpr u32 kArgumentLimit = 252;
    const u32 buffer = mMemory.read32(call.argument + kBuffer);
    const u32 length = std::min(mMemory.read32(call.argument + kArgumentLength), kArgumentLimit);
    const std::vector<std::string> arguments = readArguments(call.argument + kArguments, length);

    // Copied out, since loading writes into the memory the image sits in.
    const std::span<const u8> ram = mIop.memory().regionAt(0);
    const std::vector<u8> image =
        buffer < ram.size() ? std::vector<u8>(ram.begin() + buffer, ram.end()) : std::vector<u8>{};
    if (!isIrx(image)) {
        log.stub("module buffer at {:08x} is not an IRX", buffer);
        return static_cast<s32>(++mModules);
    }
    const std::string name = readIrxName(image);
    log.trace("LoadModuleBuffer({:08x}, {}, {} arguments)", buffer, name, arguments.size());
    // A throw would unwind through the guest's coroutine, so a module that
    // cannot be loaded is reported here and answered the way a stub is.
    std::optional<s32> result;
    try {
        result = mIop.loadModule(name.empty() ? std::format("module@{:08x}", buffer) : name,
                                 image, arguments);
    } catch (const std::exception& error) {
        log.stub("module {} at {:08x}: {}", name, buffer, error.what());
    }
    if (!result) {
        log.stub("module {} at {:08x} did not start", name, buffer);
    }
    if (call.receiveSize >= 8) {
        mMemory.write32(call.receive + kEntryResult * 4, static_cast<u32>(result.value_or(0)));
    }
    return static_cast<s32>(++mModules);
}

}  // namespace uee
