// CDVDFSV md5 89ba5280d919f5e8d20ee79b62dbe1c8.

#include "iop_cdvdfsv.h"

#include "disc.h"
#include "syscall_stubs.h"

#include <format>
#include <optional>
#include <string>
#include <vector>

namespace uee {
namespace {

// CDVDFSV: CdvdRegisterSCmdServers @ 0x44ac and CdvdRegisterNCmdServers @ 0x457c
constexpr u32 kInit = 0x80000592;
constexpr u32 kBlocking = 0x80000593;
constexpr u32 kNonBlocking = 0x80000595;
constexpr u32 kSearch = 0x80000597;
constexpr u32 kServerIds[] = {kInit, kBlocking, kNonBlocking, kSearch};

// CDVDFSV: CdvdNCmdDispatch @ 0x3f3c
constexpr u32 kRead = 1;
constexpr u32 kDiskReady = 0xE;

// CDVDFSV: CdvdSCmdDispatch @ 0x41b8
constexpr u32 kGetDiskType = 3;
constexpr u32 kGetError = 4;
constexpr u32 kBreak = 0x16;

// CDVDFSV: CdvdNCmd01_Read @ 0x4d8, SLUS-216.21: sceCdRead @ 0x510b80
constexpr u32 kReadSector = 0;
constexpr u32 kReadCount = 1;
constexpr u32 kReadBuffer = 2;
// sceCdRead zeroes sceCd_Read_cur_pos and passes its address here to count into.
constexpr u32 kReadPosition = 5;

// SLUS-216.21: sceCdLayerSearchFile @ 0x50fc38 reads the record back out of the buffer
// it sent, taking only a result code from the four byte reply.
constexpr u32 kSearchRecord = 0;
constexpr u32 kSearchSize = 4;
constexpr u32 kSearchRecordName = 8;
constexpr u32 kSearchRecordNameBytes = 16;
constexpr u32 kSearchName = 0x24;
constexpr u32 kSearchNameLimit = 256;

// SLUS-216.21: sceCdRead @ 0x510b80 treats 6 as not ready.
constexpr s32 kComplete = 2;
constexpr s32 kNoError = 0;
constexpr s32 kPlaystation2Dvd = 0x14;
constexpr s32 kFound = 1;
constexpr s32 kNotFound = 0;

}  // namespace

IopCdvdfsv::IopCdvdfsv(Memory& memory) : mMemory(memory) {}

std::span<const u32> IopCdvdfsv::serverIds() const { return kServerIds; }

s32 IopCdvdfsv::call(u32 serverId, const RpcCall& call) {
    switch (serverId) {
        case kInit:
            if (call.number == 0) {
                return kNoError;
            }
            break;
        case kBlocking:
            switch (call.number) {
                case kGetError: return kNoError;
                case kGetDiskType: return kPlaystation2Dvd;
                case kBreak: return kNoError;
                default: break;
            }
            break;
        case kNonBlocking:
            switch (call.number) {
                case kRead: return read(call.argument);
                case kDiskReady: return kComplete;
                default: break;
            }
            break;
        case kSearch:
            if (call.number == 0) {
                return search(call.argument);
            }
            break;
        default: break;
    }
    reportStub(std::format("CDVDFSV {:08x} call {}", serverId, call.number));
    return serverId == kSearch ? kNotFound : -1;
}

// CDVDFSV: CdvdNCmd01_Read @ 0x4d8. The named address is on the EE side.
s32 IopCdvdfsv::read(u32 argument) {
    if (mDisc == nullptr) {
        reportStub("disc read with no disc");
        return -1;
    }
    const u32 sector = mMemory.read32(argument + kReadSector * 4);
    const u32 count = mMemory.read32(argument + kReadCount * 4);
    const u32 buffer = mMemory.read32(argument + kReadBuffer * 4);

    std::vector<u8> data;
    if (!mDisc->read(sector, count, data)) {
        reportStub(std::format("disc read of {} sectors at {}", count, sector));
        return -1;
    }
    for (usize i = 0; i < data.size(); ++i) {
        mMemory.write8(buffer + static_cast<u32>(i), data[i]);
    }
    if (const u32 position = mMemory.read32(argument + kReadPosition * 4); position != 0) {
        mMemory.write32(position, count);
    }
    return 1;
}

// CDVDFSV: CdSearchFile @ 0x2f0
s32 IopCdvdfsv::search(u32 argument) {
    if (mDisc == nullptr) {
        reportStub("disc search with no disc");
        return kNotFound;
    }
    std::string path;
    for (u32 i = 0; i < kSearchNameLimit; ++i) {
        const char letter = static_cast<char>(mMemory.read8(argument + kSearchName + i));
        if (letter == '\0') {
            break;
        }
        path.push_back(letter);
    }

    const std::optional<DiscFile> found = mDisc->find(path);
    if (!found) {
        reportStub(std::format("disc has no {}", path));
        return kNotFound;
    }
    mMemory.write32(argument + kSearchRecord, found->sector);
    mMemory.write32(argument + kSearchSize, found->size);
    for (u32 i = 0; i < kSearchRecordNameBytes; ++i) {
        const char letter = i < found->name.size() ? found->name[i] : '\0';
        mMemory.write8(argument + kSearchRecordName + i, static_cast<u8>(letter));
    }
    return kFound;
}

}  // namespace uee
