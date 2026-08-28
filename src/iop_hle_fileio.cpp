// FILEIO md5 edf4a7d5624cb54db0cfa518d74c9559.

#include "iop_hle_fileio.h"

#include "syscall_stubs.h"

#include <format>

namespace uee {
namespace {

// FILEIO: FileIoModuleInit @ 0xc and IopHeapRpcServerThread @ 0xf70
constexpr u32 kFileIo = 0x80000001;
constexpr u32 kIopHeap = 0x80000003;
constexpr u32 kServerIds[] = {kFileIo, kIopHeap};

// FILEIO: FileIoRpcDispatch @ 0xac4
constexpr u32 kWrite = 3;

// FILEIO: IopHeapRpcDispatch @ 0xec0
constexpr u32 kAllocate = 1;
constexpr u32 kFree = 2;

constexpr u32 kDescriptor = 0;
constexpr u32 kBuffer = 1;
constexpr u32 kSize = 2;
constexpr s32 kStandardOutput = 1;
constexpr s32 kStandardError = 2;

// TODO: arbitrary, derive actual value
constexpr u32 kHeapBase = 0x00200000;

}  // namespace

IopHleFileio::IopHleFileio(Memory& memory, std::string& console)
    : mMemory(memory), mConsole(console), mHeapEnd(kHeapBase) {}

std::span<const u32> IopHleFileio::serverIds() const { return kServerIds; }

s32 IopHleFileio::call(u32 serverId, const RpcCall& call) {
    switch (serverId) {
        case kFileIo:
            switch (call.number) {
                case kWrite: return write(call.argument);
                default: break;
            }
            break;
        case kIopHeap:
            switch (call.number) {
                case kAllocate: return static_cast<s32>(allocate(mMemory.read32(call.argument)));
                case kFree: return 0;
                default: break;
            }
            break;
        default: break;
    }
    reportStub(std::format("FILEIO {:08x} call {}", serverId, call.number));
    return -1;
}

// FILEIO: FileIoWrite @ 0x568
s32 IopHleFileio::write(u32 argument) {
    const s32 descriptor = static_cast<s32>(mMemory.read32(argument + kDescriptor * 4));
    const u32 buffer = mMemory.read32(argument + kBuffer * 4);
    const u32 size = mMemory.read32(argument + kSize * 4);

    if (descriptor != kStandardOutput && descriptor != kStandardError) {
        reportStub(std::format("write to fd {}", descriptor));
        return -1;
    }

    mConsole.reserve(mConsole.size() + size);
    for (u32 i = 0; i < size; ++i) {
        mConsole.push_back(static_cast<char>(mMemory.read8(buffer + i)));
    }
    return static_cast<s32>(size);
}

// FILEIO: IopHeapAlloc @ 0xe5c
u32 IopHleFileio::allocate(u32 size) {
    const u32 block = mHeapEnd;
    mHeapEnd += (size + 0xF) & ~0xFu;
    return block;
}

}  // namespace uee
