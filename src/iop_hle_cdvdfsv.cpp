// CDVDFSV md5 89ba5280d919f5e8d20ee79b62dbe1c8.

#include "log.h"
#include "iop_hle_cdvdfsv.h"

#include "disc.h"

#include <format>
#include <optional>
#include <string>
#include <vector>

namespace uee {
namespace {

LoggerChannel log("cdvdfsv");

// CDVDFSV: CdvdRegisterSCmdServers @ 0x44ac and CdvdRegisterNCmdServers @ 0x457c
constexpr u32 kInit = 0x80000592;
constexpr u32 kBlocking = 0x80000593;
constexpr u32 kNonBlocking = 0x80000595;
constexpr u32 kSearch = 0x80000597;
// IOPRP310 CDVDFSV: CdvdRegisterDiskReadyServer @ 0x442c takes 0x8000059c, and
// CdvdDiskReadyServer @ 0x33bc answers for 0x8000059a as well.
constexpr u32 kDiskReadyA = 0x8000059A;
constexpr u32 kDiskReadyC = 0x8000059C;
constexpr u32 kServerIds[] = {kInit,        kBlocking,   kNonBlocking,
                              kSearch,      kDiskReadyA, kDiskReadyC};

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
// IOPRP310 CDVDFSV: CdvdDiskReadyServer @ 0x33bc answers 2 for a drive that has
// settled and 6 for one that has not.
constexpr s32 kDriveReady = 2;
constexpr s32 kNoError = 0;
constexpr s32 kPlaystation2Dvd = 0x14;
constexpr s32 kFound = 1;
constexpr s32 kNotFound = 0;

}  // namespace

IopHleCdvdfsv::IopHleCdvdfsv(Memory& memory) : mMemory(memory) {}

std::span<const u32> IopHleCdvdfsv::serverIds() const { return kServerIds; }

s32 IopHleCdvdfsv::call(u32 serverId, const RpcCall& call) {
    switch (serverId) {
        case kInit: return initialise(call);
        case kBlocking: return blockingCommand(call);
        case kNonBlocking: return nonBlockingCommand(call);
        case kSearch: return searchFile(call);
        case kDiskReadyA:
        case kDiskReadyC: return driveReady(call);
        default: break;
    }
    log.stub("{:08x} call {}", serverId, call.number);
    return -1;
}

// IOPRP310 CDVDFSV: CdInit @ 0x694
s32 IopHleCdvdfsv::initialise(const RpcCall& call) {
    if (call.number != 0) {
        log.stub("{:08x} call {}", kInit, call.number);
        return -1;
    }
    return kNoError;
}

// CDVDFSV: CdvdSCmdDispatch @ 0x41b8
s32 IopHleCdvdfsv::blockingCommand(const RpcCall& call) {
    switch (call.number) {
        case kGetError: return kNoError;
        case kGetDiskType:
            log.trace("CdvdSCmd03_GetDiskType() = {:02x}", kPlaystation2Dvd);
            return kPlaystation2Dvd;
        case kBreak: return kNoError;
        default: break;
    }
    log.stub("{:08x} call {}", kBlocking, call.number);
    return -1;
}

// CDVDFSV: CdvdNCmdDispatch @ 0x3f3c
s32 IopHleCdvdfsv::nonBlockingCommand(const RpcCall& call) {
    switch (call.number) {
        case kRead: return read(call.argument);
        case kDiskReady: return kComplete;
        default: break;
    }
    log.stub("{:08x} call {}", kNonBlocking, call.number);
    return -1;
}

// IOPRP310 CDVDFSV: CdSearchFile @ 0x7ac
s32 IopHleCdvdfsv::searchFile(const RpcCall& call) {
    if (call.number != 0) {
        log.stub("{:08x} call {}", kSearch, call.number);
        return kNotFound;
    }
    return search(call.argument);
}

// IOPRP310 CDVDFSV: CdvdDiskReadyServer @ 0x33bc
s32 IopHleCdvdfsv::driveReady(const RpcCall& call) {
    log.trace("CdvdDiskReadyServer(mode={}) = {}", mMemory.read32(call.argument), kDriveReady);
    return kDriveReady;
}

// CDVDFSV: CdvdNCmd01_Read @ 0x4d8. The named address is on the EE side.
s32 IopHleCdvdfsv::read(u32 argument) {
    if (mDisc == nullptr) {
        log.stub("disc read with no disc");
        return -1;
    }
    const u32 sector = mMemory.read32(argument + kReadSector * 4);
    const u32 count = mMemory.read32(argument + kReadCount * 4);
    const u32 buffer = mMemory.read32(argument + kReadBuffer * 4);
    log.trace("CdvdNCmd01_Read(sector={}, count={}, buffer={:08x})", sector, count, buffer);

    std::vector<u8> data;
    if (!mDisc->read(sector, count, data)) {
        log.stub("disc read of {} sectors at {}", count, sector);
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
s32 IopHleCdvdfsv::search(u32 argument) {
    if (mDisc == nullptr) {
        log.stub("disc search with no disc");
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
        log.stub("disc has no {}", path);
        return kNotFound;
    }
    log.trace("CdSearchFile(name={}) sector {} size {}", path, found->sector, found->size);
    mMemory.write32(argument + kSearchRecord, found->sector);
    mMemory.write32(argument + kSearchSize, found->size);
    for (u32 i = 0; i < kSearchRecordNameBytes; ++i) {
        const char letter = i < found->name.size() ? found->name[i] : '\0';
        mMemory.write8(argument + kSearchRecordName + i, static_cast<u8>(letter));
    }
    return kFound;
}

}  // namespace uee
