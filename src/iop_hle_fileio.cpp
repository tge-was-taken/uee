// A `FILEIO:` tag names the routine reimplemented below; `(310)` names the
// IOPRP310.IMG build:
//   FILEIO       md5 edf4a7d5624cb54db0cfa518d74c9559
//   FILEIO (310) md5 d84a3fa1f02e29217fac56ac1319ad64

#include "log.h"
#include "iop_hle_fileio.h"


#include <format>
#include <utility>
#include <vector>

namespace uee {
namespace {

LoggerChannel log("fileio");

// FILEIO: FileIoModuleInit @ 0xc and IopHeapRpcServerThread @ 0xf70
constexpr u32 kFileIo = 0x80000001;
constexpr u32 kIopHeap = 0x80000003;
constexpr u32 kServerIds[] = {kFileIo, kIopHeap};

// FILEIO: FileIoRpcDispatch @ 0xac4
constexpr u32 kOpen = 0;
constexpr u32 kClose = 1;
constexpr u32 kRead = 2;
constexpr u32 kWrite = 3;
constexpr u32 kSeek = 4;

// FILEIO (310): IopHeapRpcDispatch @ 0x27d4. The build in the BIOS answers only
// the first three of these.
constexpr u32 kAllocate = 1;
constexpr u32 kFree = 2;
constexpr u32 kAllocateSystem = 4;

constexpr u32 kAllocateSize = 0;
constexpr u32 kAllocateMode = 4;
constexpr u32 kAllocateAddress = 8;

constexpr u32 kDescriptor = 0;
constexpr u32 kBuffer = 1;
constexpr u32 kSize = 2;
constexpr s32 kStandardOutput = 1;
constexpr s32 kStandardError = 2;

// FILEIO: FileIoOpen @ 0x1a4 reads the flags first and the name behind them.
constexpr u32 kOpenFlags = 0;
constexpr u32 kOpenName = 4;
constexpr u32 kNameLimit = 256;

// The ioman flags FileIoOpen passes straight through.
constexpr u32 kReadAccess = 0x001;
constexpr u32 kWriteAccess = 0x002;
constexpr u32 kAppend = 0x100;
constexpr u32 kTruncate = 0x400;

// FILEIO: FileIoLseek @ 0x290
constexpr u32 kSeekOffset = 1;
constexpr u32 kSeekWhence = 2;

constexpr u32 kSeekSet = 0;
constexpr u32 kSeekCurrent = 1;
constexpr u32 kSeekEnd = 2;

std::ios::openmode modeFor(u32 flags) {
    std::ios::openmode mode = std::ios::binary;
    if ((flags & kReadAccess) != 0) {
        mode |= std::ios::in;
    }
    if ((flags & kWriteAccess) != 0) {
        mode |= std::ios::out;
    }
    if ((flags & kTruncate) != 0) {
        mode |= std::ios::trunc;
    }
    if ((flags & kAppend) != 0) {
        mode |= std::ios::app;
    }
    return mode;
}

std::string readName(Memory& memory, u32 address) {
    std::string name;
    for (u32 i = 0; i < kNameLimit; ++i) {
        const char letter = static_cast<char>(memory.read8(address + i));
        if (letter == '\0') {
            break;
        }
        name.push_back(letter);
    }
    return name;
}

}  // namespace

IopHleFileio::IopHleFileio(Memory& memory, std::string& console, Iop& iop)
    : mMemory(memory), mConsole(console), mIop(iop) {}

std::span<const u32> IopHleFileio::serverIds() const { return kServerIds; }

s32 IopHleFileio::call(u32 serverId, const RpcCall& call) {
    switch (serverId) {
        case kFileIo:
            switch (call.number) {
                case kOpen: return open(call.argument);
                case kClose: return close(call.argument);
                case kRead: return read(call.argument);
                case kWrite: return write(call.argument);
                case kSeek: return seek(call.argument);
                default: break;
            }
            break;
        case kIopHeap:
            switch (call.number) {
                case kAllocate: return allocate(call.argument);
                case kAllocateSystem: return allocateSystem(call.argument);
                case kFree: return free(call.argument);
                default: break;
            }
            break;
        default: break;
    }
    log.stub("{:08x} call {}", serverId, call.number);
    return -1;
}

// FILEIO (310): IopHeapAlloc @ 0x2578 asks sysmem for the size at the head of
// the request, from the bottom of the heap.
s32 IopHleFileio::allocate(u32 argument) {
    const u32 size = mMemory.read32(argument + kAllocateSize);
    const u32 block = mIop.allocateMemory(size);
    log.trace("allocate({} bytes) = {:08x}", size, block);
    return static_cast<s32>(block);
}

// FILEIO (310): IopHeapAllocSysMemory @ 0x25cc passes on the mode and the
// address the request carries as well as its size.
s32 IopHleFileio::allocateSystem(u32 argument) {
    const u32 size = mMemory.read32(argument + kAllocateSize);
    const u32 mode = mMemory.read32(argument + kAllocateMode);
    const u32 wanted = mMemory.read32(argument + kAllocateAddress);
    const u32 block = mIop.allocateMemory(mode, size, wanted);
    log.trace("allocate({} bytes, mode {}, at {:08x}) = {:08x}", size, mode, wanted, block);
    return static_cast<s32>(block);
}

// FILEIO (310): IopHeapFree @ 0x2624
s32 IopHleFileio::free(u32 argument) {
    const u32 block = mMemory.read32(argument);
    log.trace("free({:08x})", block);
    return mIop.freeMemory(block) ? 0 : -1;
}

std::filesystem::path IopHleFileio::resolve(std::string_view path) const {
    const usize device = path.find(':');
    if (device != std::string_view::npos) {
        path.remove_prefix(device + 1);
    }
    while (path.starts_with("./") || path.starts_with("/")) {
        path.remove_prefix(path.starts_with("./") ? 2 : 1);
    }
    return mRoot / path;
}

std::fstream* IopHleFileio::fileFor(s32 descriptor) {
    const auto found = mOpen.find(descriptor);
    return found == mOpen.end() ? nullptr : &found->second;
}

// FILEIO: FileIoOpen @ 0x1a4
s32 IopHleFileio::open(u32 argument) {
    const std::string name = readName(mMemory, argument + kOpenName);
    if (mRoot.empty()) {
        log.stub("open {} with no directory mapped", name);
        return -1;
    }

    const std::ios::openmode mode = modeFor(mMemory.read32(argument + kOpenFlags));
    const std::filesystem::path host = resolve(name);
    std::fstream file(host, mode);
    // Reading and writing together refuses to create, which is what a guest
    // asking for both without a create flag means, but writing alone creates.
    if (!file && (mode & std::ios::out) != 0 && (mode & std::ios::in) == 0) {
        file.open(host, mode | std::ios::trunc);
    }
    if (!file) {
        log.trace("open({}) missing at {}", name, host.string());
        return -1;
    }
    const s32 descriptor = mNextDescriptor++;
    mOpen.emplace(descriptor, std::move(file));
    log.trace("open({}) = {} at {}", name, descriptor, host.string());
    return descriptor;
}

// FILEIO: FileIoClose @ 0x264
s32 IopHleFileio::close(u32 argument) {
    const s32 descriptor = static_cast<s32>(mMemory.read32(argument + kDescriptor * 4));
    if (mOpen.erase(descriptor) == 0) {
        log.trace("close of {} which is not open", descriptor);
        return -1;
    }
    return 0;
}

// FILEIO: FileIoRead @ 0x2f8, which reads into a buffer of its own and sends
// that on. Here the guest's buffer is reachable, so it is filled directly.
s32 IopHleFileio::read(u32 argument) {
    const s32 descriptor = static_cast<s32>(mMemory.read32(argument + kDescriptor * 4));
    const u32 buffer = mMemory.read32(argument + kBuffer * 4);
    const u32 size = mMemory.read32(argument + kSize * 4);

    std::fstream* file = fileFor(descriptor);
    if (file == nullptr) {
        log.trace("read from {} which is not open", descriptor);
        return -1;
    }

    std::vector<char> bytes(size);
    file->read(bytes.data(), static_cast<std::streamsize>(size));
    const u32 got = static_cast<u32>(file->gcount());
    // A short read leaves eofbit set, which would refuse every later seek.
    file->clear();
    for (u32 i = 0; i < got; ++i) {
        mMemory.write8(buffer + i, static_cast<u8>(bytes[i]));
    }
    return static_cast<s32>(got);
}

// FILEIO: FileIoLseek @ 0x290
s32 IopHleFileio::seek(u32 argument) {
    const s32 descriptor = static_cast<s32>(mMemory.read32(argument + kDescriptor * 4));
    const s32 offset = static_cast<s32>(mMemory.read32(argument + kSeekOffset * 4));
    const u32 whence = mMemory.read32(argument + kSeekWhence * 4);

    std::fstream* file = fileFor(descriptor);
    if (file == nullptr) {
        log.trace("seek on {} which is not open", descriptor);
        return -1;
    }

    std::ios::seekdir direction = std::ios::beg;
    switch (whence) {
        case kSeekSet: direction = std::ios::beg; break;
        case kSeekCurrent: direction = std::ios::cur; break;
        case kSeekEnd: direction = std::ios::end; break;
        default: log.stub("seek whence {}", whence); return -1;
    }
    file->clear();
    file->seekg(offset, direction);
    file->seekp(offset, direction);
    if (!*file) {
        return -1;
    }
    return static_cast<s32>(file->tellg());
}

// FILEIO: FileIoWrite @ 0x568
s32 IopHleFileio::write(u32 argument) {
    const s32 descriptor = static_cast<s32>(mMemory.read32(argument + kDescriptor * 4));
    const u32 buffer = mMemory.read32(argument + kBuffer * 4);
    const u32 size = mMemory.read32(argument + kSize * 4);

    if (descriptor == kStandardOutput || descriptor == kStandardError) {
        mConsole.reserve(mConsole.size() + size);
        for (u32 i = 0; i < size; ++i) {
            mConsole.push_back(static_cast<char>(mMemory.read8(buffer + i)));
        }
        return static_cast<s32>(size);
    }

    std::fstream* file = fileFor(descriptor);
    if (file == nullptr) {
        log.trace("write to fd {} which is not open", descriptor);
        return -1;
    }

    std::vector<char> bytes(size);
    for (u32 i = 0; i < size; ++i) {
        bytes[i] = static_cast<char>(mMemory.read8(buffer + i));
    }
    file->write(bytes.data(), static_cast<std::streamsize>(size));
    if (!*file) {
        return -1;
    }
    return static_cast<s32>(size);
}

}  // namespace uee
