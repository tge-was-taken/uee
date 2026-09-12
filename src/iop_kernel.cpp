// A `<MODULE>: <name> @ <address>` tag names the routine reimplemented below,
// split out under ps2-split/work/ps2bios:
//   SYSCLIB  md5 6e25fddccabe0cc00aaa52afd3149cff
//   SYSMEM   md5 f313d3e9879b1d8886fb2353c816b9eb
//   INTRMANI md5 2e57f684f325b787a93f9a5883930b3b
//   THREADMAN md5 73902f937fcdc068825e9a6ac50a0355
//   SIFCMD   md5 bcdaf8bead75a7362a82c95bbbf479c7
//   SIFMAN   md5 35a6ac86ca2d7ee9c5a27de8328c3267
//   VBLANK   md5 dab9bd13305ce1c41d3b58cee21553b7
//   LIBSD    md5 4b3bb809ec3ff7c3801b5a60d7c4f744
//   LOADCORE md5 fc39f1391282c682eb9a2f03548ddf23
//   DMACMAN  md5 a204d6809d4133c688f0144d2d165d98
//   SECRMAN  md5 b8bac9b7c04d9931ddc50860497f3153
// `<MODULE> (310)` names the IOPRP310.IMG build instead:
//   SIFMAN   md5 4ba22fd5071ea28bcbe20f5d6dddfee7
// An ordinal no resident build exports is settled by a caller instead:
//   DBCMAN   md5 f6abf28d3eaf9277a7e4d35d3d2df607

#include "log.h"
#include "iop_kernel.h"
#include "iop_mmio.h"
#include "timers.h"


#include <algorithm>
#include <iterator>
#include <cstdio>
#include <format>
#include <numeric>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace uee {
namespace {

LoggerChannel log("iopkernel");

constexpr u32 kFirstArgumentRegister = 4;
constexpr u32 kResultRegister = 2;
constexpr u32 kStackRegister = 29;
constexpr u32 kGlobalRegister = 28;
// o32 reserves stack slots for the register arguments as well.
constexpr u32 kRegisterArguments = 4;

constexpr u32 kBlockAlignment = 256;

constexpr u32 kAllocFirst = 0;
constexpr u32 kAllocLast = 1;
constexpr u32 kAllocAddress = 2;

constexpr u32 kFailed = 0xFFFFFFFFu;

constexpr u32 kIllegalContext = static_cast<u32>(-100);
constexpr u32 kNoMemory = static_cast<u32>(-400);
constexpr u32 kIllegalAttr = static_cast<u32>(-401);
constexpr u32 kIllegalEntry = static_cast<u32>(-402);
constexpr u32 kIllegalPriority = static_cast<u32>(-403);
constexpr u32 kIllegalStackSize = static_cast<u32>(-404);
constexpr u32 kIllegalThid = static_cast<u32>(-406);
constexpr u32 kUnknownThid = static_cast<u32>(-407);
constexpr u32 kUnknownSemaid = static_cast<u32>(-408);
constexpr u32 kIllegalMode = static_cast<u32>(-405);
constexpr u32 kUnknownEventFlagId = static_cast<u32>(-409);
constexpr u32 kEventFlagCondition = static_cast<u32>(-421);
constexpr u32 kEventFlagPattern = static_cast<u32>(-423);

// SIFCMD: sceSifRegisterRpc @ 0x1130 fills a SifRpcServerData_t.
constexpr u32 kServerId = 0;
constexpr u32 kServerFunc = 4;
constexpr u32 kServerBuffer = 8;
constexpr u32 kServerClientFunc = 16;
constexpr u32 kServerClientBuffer = 20;

// SIFCMD: sceSifSetRpcQueue @ 0x1088 fills a SifRpcDataQueue_t.
constexpr u32 kQueueThreadId = 0;

// THREADMAN: PollEventFlag @ 0x2db0
constexpr u32 kEventFlagModeMask = 0xFFFFFFEEu;
constexpr u32 kEventFlagClear = 0x10;
constexpr u32 kNotDormant = static_cast<u32>(-414);
constexpr u32 kSemaZero = static_cast<u32>(-419);
constexpr u32 kSemaOverflow = static_cast<u32>(-420);

// THREADMAN: CreateThread @ 0xc5c
constexpr u32 kThreadAttrMask = 0x1CFFFFF7u;
constexpr s32 kLowestPriority = 126;
constexpr u32 kSmallestStack = 0x130;

// THREADMAN: CreateSema @ 0x3060
constexpr u32 kSemaAttrMask = 0xFFFFFEFEu;

constexpr u64 kMicrosecondsPerSecond = 1'000'000;
constexpr u64 kTickRatioGcd = std::gcd(kIopClockHz, kMicrosecondsPerSecond);
constexpr u64 kTicksPerMicrosecondNumerator = kIopClockHz / kTickRatioGcd;
constexpr u64 kTicksPerMicrosecondDenominator = kMicrosecondsPerSecond / kTickRatioGcd;

constexpr u32 kThreadReturn = 0x0FFFFFE0u;

// LOADCORE starts a module entry at this priority.
constexpr s32 kModulePriority = 64;

constexpr usize kHostThread = 0;

constexpr usize kMaxThreads = 64;
constexpr u64 kNoDeadline = ~0ull;
// A context of its own, since the interrupted thread's stack must survive the
// handler. Takes the top slot so guest-visible ids match hardware.
// TODO: verify with BIOS behavior
constexpr usize kInterruptThread = kMaxThreads - 1;
constexpr usize kMaxSemaphores = 64;
constexpr usize kMaxEventFlags = 64;
// DMACMAN: channel register table @ 0x1b68
// 0x1F801080 for the first seven, 0x1F801500 for the rest, 16 bytes apart.
constexpr u32 kDmaChannels = 13;
constexpr u32 kUnusableDmaChannel = 6;
constexpr u32 kDmaMadr = 0x0;
constexpr u32 kDmaBcr = 0x4;
constexpr u32 kDmaChcr = 0x8;
constexpr u32 kDmaSlice = 0x200;
constexpr u32 kDmaToMemory = 1u << 30;
constexpr u32 kDmaStart = 0x01000000;

constexpr u32 getDmaChannelBase(u32 channel) {
    return channel < 7 ? 0x1F801080 + channel * 0x10 : 0x1F801500 + (channel - 7) * 0x10;
}

constexpr u32 kSifTransferSize = 16;

constexpr u32 kIllegalIntrCode = static_cast<u32>(-101);
constexpr u32 kCpuDisabled = static_cast<u32>(-102);
constexpr u32 kFoundHandler = static_cast<u32>(-104);
constexpr u32 kNotFoundHandler = static_cast<u32>(-105);

// INTRMANI: RegisterIntrHandler @ 0x148
constexpr bool isInterruptLine(s32 line) {
    return (line >= 0 && line < 46) || line == 62 || line == 63;
}

u32 readRegister(const std::map<u32, u32>& bank, u32 entry) {
    const auto found = bank.find(entry);
    return found == bank.end() ? 0 : found->second;
}

constexpr std::string_view kSpecFlags = "-+ #0";
constexpr std::string_view kSpecDigits = "0123456789.";
constexpr std::string_view kSpecLengths = "lhzjt";

constexpr u8 kUpper = 0x01;
constexpr u8 kLower = 0x02;
constexpr u8 kDigit = 0x04;
constexpr u8 kSpace = 0x08;
constexpr u8 kPunct = 0x10;
constexpr u8 kControl = 0x20;
constexpr u8 kHex = 0x40;

// SYSCLIB: ctype table @ 0x1af1
constexpr u8 classify(u32 c) {
    u8 flags = 0;
    if (c >= 'A' && c <= 'Z') {
        flags |= kUpper;
    }
    if (c >= 'a' && c <= 'z') {
        flags |= kLower;
    }
    if (c >= '0' && c <= '9') {
        flags |= kDigit;
    }
    if ((c >= 9 && c <= 13) || c == ' ') {
        flags |= kSpace;
    }
    if (c == ' ' || (c >= 33 && c <= 47) || (c >= 58 && c <= 64) || (c >= 91 && c <= 96) ||
        (c >= 123 && c <= 126)) {
        flags |= kPunct;
    }
    if (c < 9 || (c >= 14 && c <= 31) || c == 127) {
        flags |= kControl;
    }
    if ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
        flags |= kHex;
    }
    return flags;
}

u32 digitValue(u8 c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'Z') {
        return c - 'A' + 10;
    }
    return 0xFFFFFFFFu;
}

bool eventFlagSatisfied(u32 bits, u32 pattern, u32 mode) {
    return (mode & 1) != 0 ? (bits & pattern) != 0 : (bits & pattern) == pattern;
}

template <typename T>
void appendFormatted(std::string& out, const std::string& spec, T value) {
    const int size = std::snprintf(nullptr, 0, spec.c_str(), value);
    if (size < 0) {
        throw std::runtime_error(std::format("the host rejected the format {}", spec));
    }
    std::string text(static_cast<usize>(size), '\0');
    std::snprintf(text.data(), static_cast<usize>(size) + 1, spec.c_str(), value);
    out += text;
}

}  // namespace

std::span<const IopKernel::Service> IopKernel::services() {
    static constexpr Service kServices[] = {
        {"stdio", 4, "printf", &IopKernel::printf},
        {"loadcore", 4, "FlushIcache", &IopKernel::flushCache, true},
        {"loadcore", 5, "FlushDcache", &IopKernel::flushCache, true},
        {"loadcore", 6, "RegisterLibraryEntries", &IopKernel::registerLibraryEntries, true},
        {"loadcore", 12, "QueryBootMode", &IopKernel::queryBootMode, true},
        {"dmacman", 33, "sceSetDMAPriority", &IopKernel::setDmaPriority, true},
        {"dmacman", 28, "dmacRequest", &IopKernel::requestDma},
        {"dmacman", 32, "dmacStartTransfer", &IopKernel::startDma},
        {"dmacman", 34, "sceEnableDMAChannel", &IopKernel::enableDmaChannel, true},
        {"loadcore", 27, "SetRebootTimeLibraryHandlingMode",
         &IopKernel::setRebootTimeLibraryHandlingMode, true},
        {"secrman", 5, "SecrSetMcDevIDHandler", &IopKernel::setMcDevIdHandler, true},
        {"secrman", 6, "SecrAuthCard", &IopKernel::authCard},
        {"secrman", 4, "SecrSetMcCommandHandler", &IopKernel::setMcCommandHandler, true},
        {"sysmem", 14, "Kprintf", &IopKernel::kprintf},
        {"thbase", 4, "CreateThread", &IopKernel::createThread},
        {"thbase", 5, "DeleteThread", &IopKernel::deleteThread},
        {"thbase", 6, "StartThread", &IopKernel::startThread},
        {"thbase", 8, "ExitThread", &IopKernel::exitThread},
        {"thbase", 10, "TerminateThread", &IopKernel::terminateThread},
        {"thbase", 14, "ChangeThreadPriority", &IopKernel::changeThreadPriority},
        {"thbase", 20, "GetThreadId", &IopKernel::getThreadId},
        {"thbase", 22, "ReferThreadStatus", &IopKernel::referThreadStatus},
        {"thbase", 24, "SleepThread", &IopKernel::sleepThread},
        {"thbase", 25, "WakeupThread", &IopKernel::wakeupThread},
        {"thbase", 26, "iWakeupThread", &IopKernel::wakeupThread},
        {"thbase", 27, "CancelWakeupThread", &IopKernel::cancelWakeupThread},
        {"thbase", 28, "iCancelWakeupThread", &IopKernel::cancelWakeupThread},
        {"thbase", 33, "DelayThread", &IopKernel::delayThread, true},
        {"thbase", 34, "GetSystemTime", &IopKernel::getSystemTime},
        {"thbase", 39, "USec2SysClock", &IopKernel::usecToSysClock},
        {"thbase", 40, "SysClock2USec", &IopKernel::sysClockToUsec},
        {"thsemap", 4, "CreateSema", &IopKernel::createSema},
        {"thsemap", 5, "DeleteSema", &IopKernel::deleteSema},
        {"thsemap", 6, "SignalSema", &IopKernel::signalSema},
        {"thsemap", 7, "iSignalSema", &IopKernel::signalSema},
        {"thsemap", 8, "WaitSema", &IopKernel::waitSema},
        {"thsemap", 9, "PollSema", &IopKernel::pollSema},
        {"thsemap", 11, "ReferSemaStatus", &IopKernel::referSemaStatus},
        {"thevent", 4, "CreateEventFlag", &IopKernel::createEventFlag},
        {"thevent", 5, "DeleteEventFlag", &IopKernel::deleteEventFlag},
        {"thevent", 6, "SetEventFlag", &IopKernel::setEventFlag},
        {"thevent", 7, "iSetEventFlag", &IopKernel::setEventFlag},
        {"thevent", 8, "ClearEventFlag", &IopKernel::clearEventFlag},
        {"thevent", 9, "iClearEventFlag", &IopKernel::clearEventFlag},
        {"thevent", 10, "WaitEventFlag", &IopKernel::waitEventFlag},
        {"thevent", 11, "PollEventFlag", &IopKernel::pollEventFlag},
        {"thevent", 12, "iPollEventFlag", &IopKernel::pollEventFlag},
        {"thevent", 13, "ReferEventFlagStatus", &IopKernel::referEventFlagStatus},
        {"thevent", 14, "iReferEventFlagStatus", &IopKernel::referEventFlagStatus},
        {"intrman", 4, "RegisterIntrHandler", &IopKernel::registerIntrHandler, true},
        {"intrman", 5, "ReleaseIntrHandler", &IopKernel::releaseIntrHandler, true},
        {"intrman", 6, "EnableIntr", &IopKernel::enableIntr, true},
        {"intrman", 7, "DisableIntr", &IopKernel::disableIntr, true},
        {"intrman", 8, "CpuDisableIntr", &IopKernel::cpuDisableIntr},
        {"intrman", 9, "CpuEnableIntr", &IopKernel::cpuEnableIntr},
        {"intrman", 17, "CpuSuspendIntr", &IopKernel::cpuSuspendIntr},
        {"intrman", 18, "CpuResumeIntr", &IopKernel::cpuResumeIntr},
        {"intrman", 23, "QueryIntrContext", &IopKernel::queryIntrContext},
        {"sysmem", 4, "AllocSysMemory", &IopKernel::allocSysMemory},
        {"sysmem", 5, "FreeSysMemory", &IopKernel::freeSysMemory},
        {"sysmem", 6, "QueryMemSize", &IopKernel::queryMemSize},
        {"sysmem", 7, "QueryMaxFreeMemSize", &IopKernel::queryMaxFreeMemSize},
        {"sysmem", 8, "QueryTotalFreeMemSize", &IopKernel::queryTotalFreeMemSize},
        {"sysmem", 9, "QueryBlockTopAddress", &IopKernel::queryBlockTopAddress},
        {"sysmem", 10, "QueryBlockSize", &IopKernel::queryBlockSize},
        {"sysclib", 8, "look_ctype_table", &IopKernel::lookCtypeTable},
        {"sysclib", 11, "memcmp", &IopKernel::memcmp},
        {"sysclib", 12, "memcpy", &IopKernel::memcpy},
        {"sysclib", 14, "memset", &IopKernel::memset},
        {"sysclib", 17, "bzero", &IopKernel::bzero},
        {"sysclib", 19, "sprintf", &IopKernel::sprintf},
        {"sysclib", 20, "strcat", &IopKernel::strcat},
        {"sysclib", 23, "strcpy", &IopKernel::strcpy},
        {"sysclib", 27, "strlen", &IopKernel::strlen},
        {"sysclib", 29, "strncmp", &IopKernel::strncmp},
        {"sysclib", 30, "strncpy", &IopKernel::strncpy},
        {"sysclib", 36, "strtol", &IopKernel::strtol},
        {"sifman", 5, "sceSifInit", &IopKernel::sifInit, true},
        {"sifman", 7, "sceSifSetDma", &IopKernel::sifSetDma},
        {"sifman", 32, "sceSifSetDmaIntr", &IopKernel::sifSetDmaIntr},
        {"sifman", 29, "sceSifCheckInit", &IopKernel::sifCheckInit, true},
        {"sifcmd", 14, "sceSifInitRpc", &IopKernel::sifInitRpc, true},
        {"sifman", 8, "sceSifDmaStat", &IopKernel::sifDmaStat, true},
        {"sifcmd", 15, "sceSifBindRpc", &IopKernel::sifBindRpc, true},
        {"sifcmd", 16, "sceSifCallRpc", &IopKernel::sifCallRpc, true},
        {"sifcmd", 17, "sceSifRegisterRpc", &IopKernel::sifRegisterRpc},
        {"sifcmd", 19, "sceSifSetRpcQueue", &IopKernel::sifSetRpcQueue},
        {"sifcmd", 22, "sceSifRpcLoop", &IopKernel::sifRpcLoop, true},
        {"sifcmd", 23, "sceSifGetOtherData", &IopKernel::sifGetOtherData},
        {"sifcmd", 24, "sceSifRemoveRpc", &IopKernel::sifRemoveRpc},
        {"sifcmd", 25, "sceSifRemoveRpcQueue", &IopKernel::sifRemoveRpcQueue, true},
        {"vblank", 5, "WaitVblankEnd", &IopKernel::waitVblankEnd, true},
        {"vblank", 8, "RegisterVblankHandler", &IopKernel::registerVblankHandler, true},
        {"vblank", 9, "ReleaseVblankHandler", &IopKernel::releaseVblankHandler, true},
        // IOPRP310 TIMEMANI md5 9b39d117edce1d23f9383cf8a7c5d11d
        {"timrman", 4, "AllocHardTimer", &IopKernel::allocHardTimer},
        {"timrman", 6, "FreeHardTimer", &IopKernel::freeHardTimer},
        {"timrman", 20, "SetTimerHandler", &IopKernel::setTimerHandler},
        {"timrman", 22, "SetupHardTimer", &IopKernel::setupHardTimer},
        {"timrman", 23, "StartHardTimer", &IopKernel::startHardTimer, true},
        {"timrman", 24, "StopHardTimer", &IopKernel::stopHardTimer},
        {"libsd", 4, "sceSdInit", &IopKernel::sdInit, true},
        {"libsd", 5, "sceSdSetParam", &IopKernel::sdSetParam},
        {"libsd", 6, "sceSdGetParam", &IopKernel::sdGetParam},
        {"libsd", 7, "sceSdSetSwitch", &IopKernel::sdSetSwitch},
        {"libsd", 8, "sceSdGetSwitch", &IopKernel::sdGetSwitch},
        {"libsd", 9, "sceSdSetAddr", &IopKernel::sdSetAddr},
        {"libsd", 10, "sceSdGetAddr", &IopKernel::sdGetAddr},
        {"libsd", 11, "sceSdSetCoreAttr", &IopKernel::sdSetCoreAttr},
        {"libsd", 12, "sceSdGetCoreAttr", &IopKernel::sdGetCoreAttr},
        {"libsd", 13, "sceSdNote2Pitch", &IopKernel::sdConvertPitch, true},
        {"libsd", 14, "sceSdPitch2Note", &IopKernel::sdConvertPitch, true},
        {"libsd", 15, "sceSdProcBatch", &IopKernel::sdProcBatch, true},
        {"libsd", 16, "sceSdProcBatchEx", &IopKernel::sdProcBatch, true},
        {"libsd", 23, "sceSdSetEffectAttr", &IopKernel::sdSetEffectAttr, true},
        {"libsd", 24, "sceSdGetEffectAttr", &IopKernel::sdGetEffectAttr, true},
        {"libsd", 25, "sceSdClearEffectWorkArea", &IopKernel::sdClearEffectWorkArea, true},
        {"libsd", 27, "sceSdSetSpu2IntrHandler", &IopKernel::sdSetSpu2IntrHandler},
        {"libsd", 30, "sceSdVoiceTransWrite", &IopKernel::sdAccepted, true},
        {"libsd", 31, "sceSdBlockTransWrite", &IopKernel::sdAccepted, true},
        {"libsd", 32, "sceSdEffectAttrApply", &IopKernel::sdAccepted, true},
        {"libsd", 33, "sceSdEffectAttrRead", &IopKernel::sdAccepted, true},
        {"libsd", 17, "sceSdVoiceTrans", &IopKernel::sdVoiceTrans, true},
        {"libsd", 18, "sceSdBlockTrans", &IopKernel::sdBlockTrans, true},
        {"libsd", 19, "sceSdVoiceTransStatus", &IopKernel::sdTransStatus, true},
        {"libsd", 20, "sceSdBlockTransStatus", &IopKernel::sdTransStatus, true},
        {"libsd", 26, "sceSdSetTransIntrHandler", &IopKernel::sdSetTransIntrHandler, true},
        {"libsd", 28, "sceSdGetTransIntrHandlerArgument", &IopKernel::sdGetTransIntrHandlerArgument, true},
    };
    return kServices;
}

const IopKernel::Service* IopKernel::lookup(const Import& import) {
    for (const Service& service : services()) {
        if (service.module == import.module && service.id == import.id) {
            return &service;
        }
    }
    return nullptr;
}

std::optional<u32> IopKernel::findService(std::string_view module, u32 id) {
    const std::span<const Service> table = services();
    for (u32 i = 0; i < table.size(); ++i) {
        if (table[i].module == module && table[i].id == id) {
            return i;
        }
    }
    return std::nullopt;
}

void IopKernel::callService(u32 index) {
    const Service& service = services()[index];
    if (service.isStub && mStubReported.insert(&service).second) {
        log.stub("{}.{}", service.module, service.symbol);
    }
    (this->*service.run)();
}

IopKernel::IopKernel(IopContext& iop, Memory& memory, Memory& mainMemory,
                     const std::vector<Import>& imports, HeapRange heap, std::string& console)
    : mIop(iop), mMemory(memory), mMainMemory(mainMemory), mImports(imports),
      mConsole(console) {
    mThreads.resize(kMaxThreads);
    mSemaphores.resize(kMaxSemaphores);
    mEventFlags.resize(kMaxEventFlags);
    mThreads[kHostThread].allocated = true;
    mThreads[kHostThread].sequence = ++mSequence;
    mThreads[kHostThread].status = iopstatus::kRun;
    mThreads[kHostThread].initialPriority = kModulePriority;
    mThreads[kHostThread].currentPriority = kModulePriority;
    mThreads[kInterruptThread].allocated = true;
    mThreads[kInterruptThread].status = iopstatus::kDormant;

    const u32 start = (heap.start + kBlockAlignment - 1) & ~(kBlockAlignment - 1);
    const u32 end = heap.end & ~(kBlockAlignment - 1);
    if (end > start) {
        mBlocks.push_back({start, end - start, false});
    }
}

std::string IopKernel::nameOf(u32 index) const {
    if (index >= mImports.size()) {
        return std::format("import {}", index);
    }
    const Service* service = index < mServices.size() ? mServices[index] : nullptr;
    if (service == nullptr) {
        return std::format("{}.{}", mImports[index].module, mImports[index].id);
    }
    return std::format("{}.{}", service->module, service->symbol);
}

bool IopKernel::call(u32 index) {
    while (mServices.size() < mImports.size()) {
        const Import& want = mImports[mServices.size()];
        const Service* found = lookup(want);
        if (found == nullptr) {
            log.stub("nothing answers {}.{}", want.module, want.id);
        }
        mServices.push_back(found);
    }
    if (index >= mServices.size() || mServices[index] == nullptr) {
        return false;
    }
    const Service& service = *mServices[index];
    if (service.isStub && mStubReported.insert(&service).second) {
        log.stub("{}.{}", service.module, service.symbol);
    }
    const std::string call =
        log.tracing()
            ? std::format("{}.{}({:08x}, {:08x}, {:08x}, {:08x})", service.module, service.symbol,
                          argument(0), argument(1), argument(2), argument(3))
            : std::string{};
    (this->*service.run)();
    log.trace("{} = {:08x}", call, mIop.cpu.gpr[kResultRegister]);
    return true;
}

u32 IopKernel::argument(u32 index) {
    if (index < kRegisterArguments) {
        return mIop.cpu.gpr[kFirstArgumentRegister + index];
    }
    return mMemory.read32(mIop.cpu.gpr[kStackRegister] + 4 * index);
}

void IopKernel::returnValue(u32 value) {
    mIop.cpu.write(kResultRegister, value);
}

u32 IopKernel::stringLength(u32 address) {
    for (u32 i = 0; i < kIopRamSize; ++i) {
        if (mMemory.read8(address + i) == 0) {
            return i;
        }
    }
    throw std::runtime_error(std::format("the string at {:08x} is not terminated", address));
}

std::string IopKernel::readString(u32 address) {
    const u32 length = stringLength(address);
    std::string text;
    text.reserve(length);
    for (u32 i = 0; i < length; ++i) {
        text.push_back(static_cast<char>(mMemory.read8(address + i)));
    }
    return text;
}

void IopKernel::writeString(u32 address, const std::string& text) {
    for (usize i = 0; i < text.size(); ++i) {
        mMemory.write8(address + static_cast<u32>(i), static_cast<u8>(text[i]));
    }
    mMemory.write8(address + static_cast<u32>(text.size()), 0);
}

std::string IopKernel::format(u32 text, u32 firstArgument) {
    const std::string source = readString(text);
    std::string out;
    u32 next = firstArgument;

    for (usize i = 0; i < source.size(); ++i) {
        if (source[i] != '%') {
            out.push_back(source[i]);
            continue;
        }
        const usize start = i;
        ++i;
        while (i < source.size() && kSpecFlags.find(source[i]) != std::string_view::npos) {
            ++i;
        }
        while (i < source.size() && kSpecDigits.find(source[i]) != std::string_view::npos) {
            ++i;
        }
        const usize afterWidth = i;
        while (i < source.size() && kSpecLengths.find(source[i]) != std::string_view::npos) {
            ++i;
        }
        if (i >= source.size()) {
            throw std::runtime_error(std::format("the format {} ends mid directive", source));
        }
        // A host long is 64 bits, so the guest's length modifier cannot go through.
        const std::string spec = source.substr(start, afterWidth - start) + source[i];

        switch (source[i]) {
            case '%':
                out.push_back('%');
                break;
            case 'd': case 'i':
                appendFormatted(out, spec, static_cast<s32>(argument(next++)));
                break;
            // %p omitted: the host would read 64 bits off a 32-bit argument.
            case 'u': case 'x': case 'X': case 'o':
                appendFormatted(out, spec, argument(next++));
                break;
            case 'c':
                appendFormatted(out, spec, static_cast<int>(argument(next++)));
                break;
            case 's': {
                const std::string value = readString(argument(next++));
                appendFormatted(out, spec, value.c_str());
                break;
            }
            default:
                throw std::runtime_error(std::format(
                    "the conversion {} in {} is not one this stands in for", source[i], source));
        }
    }
    return out;
}

void IopKernel::printf() {
    const std::string text = format(argument(0), 1);
    mConsole += text;
    returnValue(static_cast<u32>(text.size()));
}

// SYSCLIB: sprintf @ 0x1750
void IopKernel::sprintf() {
    const std::string text = format(argument(1), 2);
    writeString(argument(0), text);
    returnValue(static_cast<u32>(text.size()));
}

// SYSCLIB: look_ctype_table @ 0xa00
void IopKernel::lookCtypeTable() {
    const u32 c = argument(0);
    returnValue(c < 128 ? classify(c) : 0);
}

// SYSCLIB: memcmp @ 0xa68
void IopKernel::memcmp() {
    const u32 left = argument(0);
    const u32 right = argument(1);
    const s32 size = static_cast<s32>(argument(2));
    for (s32 i = 0; i < size; ++i) {
        const u8 a = mMemory.read8(left + static_cast<u32>(i));
        const u8 b = mMemory.read8(right + static_cast<u32>(i));
        if (a != b) {
            returnValue(static_cast<u32>(a < b ? -1 : 1));
            return;
        }
    }
    returnValue(0);
}

void IopKernel::memcpy() {
    const u32 destination = argument(0);
    const u32 source = argument(1);
    const u32 size = argument(2);
    for (u32 i = 0; i < size; ++i) {
        mMemory.write8(destination + i, mMemory.read8(source + i));
    }
    returnValue(destination);
}

void IopKernel::memset() {
    const u32 destination = argument(0);
    const u8 value = static_cast<u8>(argument(1));
    const u32 size = argument(2);
    for (u32 i = 0; i < size; ++i) {
        mMemory.write8(destination + i, value);
    }
    returnValue(destination);
}

void IopKernel::bzero() {
    const u32 destination = argument(0);
    const u32 size = argument(1);
    for (u32 i = 0; i < size; ++i) {
        mMemory.write8(destination + i, 0);
    }
}

// SYSCLIB: strcat @ 0xccc
void IopKernel::strcat() {
    const u32 destination = argument(0);
    const u32 source = argument(1);
    if (destination == 0) {
        returnValue(0);
        return;
    }
    if (source != 0) {
        writeString(destination + stringLength(destination), readString(source));
    }
    returnValue(destination);
}

// SYSCLIB: strcpy @ 0xe0c
void IopKernel::strcpy() {
    const u32 destination = argument(0);
    const u32 source = argument(1);
    if (destination == 0 || source == 0) {
        returnValue(0);
        return;
    }
    writeString(destination, readString(source));
    returnValue(destination);
}

// SYSCLIB: strlen @ 0xf54
void IopKernel::strlen() {
    const u32 text = argument(0);
    returnValue(text == 0 ? 0 : stringLength(text));
}

// SYSCLIB: strncmp @ 0xff4
void IopKernel::strncmp() {
    const u32 left = argument(0);
    const u32 right = argument(1);
    const s32 size = static_cast<s32>(argument(2));
    if (left == 0 || right == 0) {
        returnValue(left == right ? 0 : static_cast<u32>(left == 0 ? -1 : 1));
        return;
    }
    for (s32 i = 0; i < size; ++i) {
        const s8 a = static_cast<s8>(mMemory.read8(left + static_cast<u32>(i)));
        const s8 b = static_cast<s8>(mMemory.read8(right + static_cast<u32>(i)));
        if (a != b) {
            returnValue(static_cast<u32>(a - b));
            return;
        }
        if (a == 0) {
            break;
        }
    }
    returnValue(0);
}

// SYSCLIB: strncpy @ 0x107c
void IopKernel::strncpy() {
    const u32 destination = argument(0);
    const u32 source = argument(1);
    const s32 size = static_cast<s32>(argument(2));
    if (destination == 0 || source == 0) {
        returnValue(0);
        return;
    }
    s32 i = 0;
    for (; i < size; ++i) {
        const u8 c = mMemory.read8(source + static_cast<u32>(i));
        mMemory.write8(destination + static_cast<u32>(i), c);
        if (c == 0) {
            break;
        }
    }
    for (; i < size; ++i) {
        mMemory.write8(destination + static_cast<u32>(i), 0);
    }
    returnValue(destination);
}

// SYSCLIB: strtol @ 0x1398
void IopKernel::strtol() {
    const u32 start = argument(0);
    const u32 end = argument(1);
    u32 base = argument(2);

    u32 at = start;
    while ((classify(mMemory.read8(at)) & kSpace) != 0) {
        ++at;
    }
    bool negative = false;
    if (mMemory.read8(at) == '+' || mMemory.read8(at) == '-') {
        negative = mMemory.read8(at) == '-';
        ++at;
    }
    if ((base == 0 || base == 16) && mMemory.read8(at) == '0' &&
        (mMemory.read8(at + 1) | 0x20) == 'x') {
        base = 16;
        at += 2;
    } else if (base == 0) {
        base = mMemory.read8(at) == '0' ? 8 : 10;
    }

    const u32 first = at;
    u32 value = 0;
    while (digitValue(mMemory.read8(at)) < base) {
        value = value * base + digitValue(mMemory.read8(at));
        ++at;
    }
    if (end != 0) {
        mMemory.write32(end, at == first ? start : at);
    }
    returnValue(negative ? ~value + 1 : value);
}

std::vector<IopKernel::Block>::iterator IopKernel::blockAt(u32 address) {
    return std::find_if(mBlocks.begin(), mBlocks.end(),
                        [address](const Block& block) { return block.start == address; });
}

std::vector<IopKernel::Block>::iterator IopKernel::blockContaining(u32 address) {
    return std::find_if(mBlocks.begin(), mBlocks.end(), [address](const Block& block) {
        return address >= block.start && address < block.start + block.size;
    });
}

// SYSMEM: AllocSysMemory @ 0x2ec
void IopKernel::allocSysMemory() {
    returnValue(allocateBlock(argument(0), argument(1), argument(2)));
}

u32 IopKernel::allocateBlock(u32 mode, u32 requested, u32 wanted) {
    const u32 size = (requested + kBlockAlignment - 1) & ~(kBlockAlignment - 1);
    if (mode > kAllocAddress || size == 0) {
        return 0;
    }

    auto chosen = mBlocks.end();
    for (auto block = mBlocks.begin(); block != mBlocks.end(); ++block) {
        if (block->used || block->size < size) {
            continue;
        }
        if (mode == kAllocAddress && block->start > wanted) {
            continue;
        }
        if (chosen == mBlocks.end() || mode != kAllocFirst) {
            chosen = block;
        }
        if (mode == kAllocFirst) {
            break;
        }
    }
    if (chosen == mBlocks.end()) {
        return 0;
    }

    const u32 start = mode == kAllocFirst ? chosen->start : chosen->start + chosen->size - size;
    const u32 remaining = chosen->size - size;
    if (remaining == 0) {
        chosen->used = true;
    } else if (mode == kAllocFirst) {
        chosen->size = remaining;
        chosen->start += size;
        mBlocks.insert(chosen, {start, size, true});
    } else {
        chosen->size = remaining;
        mBlocks.insert(chosen + 1, {start, size, true});
    }
    return start;
}

// SYSMEM: FreeSysMemory @ 0x33c over HeapFreeBlock @ 0x86c
void IopKernel::freeSysMemory() {
    returnValue(freeBlock(argument(0)) ? 0 : kFailed);
}

bool IopKernel::freeBlock(u32 address) {
    const auto block = blockAt(address);
    if ((address % kBlockAlignment) != 0 || block == mBlocks.end() || !block->used) {
        return false;
    }
    block->used = false;
    for (usize i = 0; i + 1 < mBlocks.size();) {
        if (!mBlocks[i].used && !mBlocks[i + 1].used) {
            mBlocks[i].size += mBlocks[i + 1].size;
            mBlocks.erase(mBlocks.begin() + static_cast<isize>(i) + 1);
        } else {
            ++i;
        }
    }
    return true;
}

// SYSMEM: QueryMemSize @ 0x204
void IopKernel::queryMemSize() {
    returnValue(kIopRamSize);
}

// SYSMEM: QueryMaxFreeMemSize @ 0x230
void IopKernel::queryMaxFreeMemSize() {
    u32 largest = 0;
    for (const Block& block : mBlocks) {
        if (!block.used && block.size > largest) {
            largest = block.size;
        }
    }
    returnValue(largest);
}

// SYSMEM: QueryTotalFreeMemSize @ 0x294
void IopKernel::queryTotalFreeMemSize() {
    u32 total = 0;
    for (const Block& block : mBlocks) {
        if (!block.used) {
            total += block.size;
        }
    }
    returnValue(total);
}

// SYSMEM: QueryBlockSize @ 0x40c over HeapBlockInfo @ 0xb28
void IopKernel::queryBlockSize() {
    const auto block = blockContaining(argument(0));
    if (block == mBlocks.end()) {
        returnValue(kFailed);
        return;
    }
    returnValue(block->used ? block->size : block->size | 0x80000000u);
}

// SYSMEM: QueryBlockTopAddress @ 0x3a4 over HeapBlockInfo @ 0xb28
void IopKernel::queryBlockTopAddress() {
    const auto block = blockContaining(argument(0));
    if (block == mBlocks.end()) {
        returnValue(kFailed);
        return;
    }
    returnValue(block->used ? block->start : block->start + 0x80000000u);
}

// INTRMANI: CpuSuspendIntr @ 0x348
void IopKernel::cpuSuspendIntr() {
    const u32 state = argument(0);
    const bool was = mInterruptsEnabled;
    if (state != 0) {
        mMemory.write32(state, was ? 1 : 0);
    }
    mInterruptsEnabled = false;
    returnValue(was ? 0 : kCpuDisabled);
}

// INTRMANI: CpuResumeIntr @ 0x374
void IopKernel::cpuResumeIntr() {
    mInterruptsEnabled = argument(0) != 0;
    returnValue(0);
}

void IopKernel::cpuEnableIntr() {
    mInterruptsEnabled = true;
    returnValue(0);
}

void IopKernel::cpuDisableIntr() {
    const bool was = mInterruptsEnabled;
    mInterruptsEnabled = false;
    returnValue(was ? 0 : kCpuDisabled);
}

// INTRMANI: QueryIntrContext @ 0xd70
void IopKernel::queryIntrContext() {
    returnValue(0);
}

// INTRMANI: RegisterIntrHandler @ 0x148
void IopKernel::registerIntrHandler() {
    const s32 line = static_cast<s32>(argument(0));
    const u32 handler = argument(2);
    if (!isInterruptLine(line) || handler == 0) {
        returnValue(kIllegalIntrCode);
        return;
    }
    if (mInterrupts[line].handler != 0) {
        returnValue(kFoundHandler);
        return;
    }
    mInterrupts[line] = {handler, argument(3), mIop.cpu.gpr[reg::gp]};
    returnValue(0);
}

// INTRMANI: ReleaseIntrHandler @ 0x270
void IopKernel::releaseIntrHandler() {
    const s32 line = static_cast<s32>(argument(0));
    if (!isInterruptLine(line)) {
        returnValue(kIllegalIntrCode);
        return;
    }
    if (mInterrupts[line].handler == 0) {
        returnValue(kNotFoundHandler);
        return;
    }
    mInterrupts[line] = {};
    returnValue(0);
}

void IopKernel::enableIntr() {
    returnValue(isInterruptLine(static_cast<s32>(argument(0))) ? 0 : kIllegalIntrCode);
}

void IopKernel::disableIntr() {
    returnValue(isInterruptLine(static_cast<s32>(argument(0))) ? 0 : kIllegalIntrCode);
}

u32 IopKernel::makeHandle(usize index, u32 sequence) {
    return (static_cast<u32>(index) << 7) | ((sequence & 0x3F) << 1) | 1;
}

usize IopKernel::handleIndex(u32 handle) {
    return handle >> 7;
}

u32 IopKernel::handleSequence(u32 handle) {
    return (handle >> 1) & 0x3F;
}

IopThread* IopKernel::threadFor(u32 handle) {
    const usize index = handleIndex(handle);
    if ((handle & 1) == 0 || index >= mThreads.size() || !mThreads[index].allocated) {
        return nullptr;
    }
    IopThread& thread = mThreads[index];
    return (thread.sequence & 0x3F) == handleSequence(handle) ? &thread : nullptr;
}

IopSemaphore* IopKernel::semaphoreFor(u32 handle) {
    const usize index = handleIndex(handle);
    if ((handle & 1) == 0 || index >= mSemaphores.size() || !mSemaphores[index].allocated) {
        return nullptr;
    }
    IopSemaphore& semaphore = mSemaphores[index];
    return (semaphore.sequence & 0x3F) == handleSequence(handle) ? &semaphore : nullptr;
}

IopEventFlag* IopKernel::eventFlagFor(u32 handle) {
    const usize index = handleIndex(handle);
    if ((handle & 1) == 0 || index >= mEventFlags.size() || !mEventFlags[index].allocated) {
        return nullptr;
    }
    IopEventFlag& flag = mEventFlags[index];
    return (flag.sequence & 0x3F) == handleSequence(handle) ? &flag : nullptr;
}

void IopKernel::switchTo(usize thread) {
    if (thread == mCurrent) {
        mThreads[thread].status = iopstatus::kRun;
        return;
    }
    IopThread& outgoing = mThreads[mCurrent];
    if (outgoing.status == iopstatus::kRun) {
        outgoing.status = iopstatus::kReady;
    }
    outgoing.saved = mIop.cpu;
    mCurrent = thread;
    mThreads[thread].status = iopstatus::kRun;
    mIop.cpu = mThreads[thread].saved;
}

u32 IopKernel::waitingOn(IopWait wait, u32 id) const {
    u32 waiting = 0;
    for (const IopThread& thread : mThreads) {
        if (thread.status == iopstatus::kWait && thread.wait == wait && thread.waitId == id) {
            ++waiting;
        }
    }
    return waiting;
}

void IopKernel::readyThread(usize thread) {
    mThreads[thread].status = iopstatus::kReady;
    mThreads[thread].wait = IopWait::None;
    mThreads[thread].waitId = 0;
}

void IopKernel::blockCurrentThread(IopWait wait, u32 id) {
    IopThread& thread = mThreads[mCurrent];
    thread.status = iopstatus::kWait;
    thread.wait = wait;
    thread.waitId = id;
    reschedule();
}

void IopKernel::reschedule() {
    usize best = mThreads.size();
    for (usize i = 0; i < mThreads.size(); ++i) {
        const IopThread& thread = mThreads[i];
        if (!thread.allocated ||
            (thread.status & (iopstatus::kReady | iopstatus::kRun)) == 0) {
            continue;
        }
        if (best == mThreads.size() || thread.currentPriority < mThreads[best].currentPriority) {
            best = i;
        }
    }
    if (best != mThreads.size()) {
        switchTo(best);
    }
}

bool IopKernel::isCurrent(usize thread) const {
    return mCurrent == thread && mThreads[thread].status == iopstatus::kRun;
}

bool IopKernel::isHostWaitingOnEe() const {
    const IopThread& host = mThreads[kHostThread];
    return host.status == iopstatus::kWait && host.wait == IopWait::RpcReply;
}

void IopKernel::enterHostThread() {
    mThreads[kHostThread].status = iopstatus::kRun;
    switchTo(kHostThread);
}

usize IopKernel::enterInterruptThread() {
    const usize interrupted = mCurrent;
    mThreads[kInterruptThread].status = iopstatus::kRun;
    switchTo(kInterruptThread);
    return interrupted;
}

// The interrupted thread gets its registers back before anything preempts it.
void IopKernel::leaveInterruptThread(usize interrupted) {
    mThreads[kInterruptThread].status = iopstatus::kDormant;
    mCurrent = interrupted;
    mIop.cpu = mThreads[interrupted].saved;
    if (mThreads[interrupted].status == iopstatus::kReady) {
        mThreads[interrupted].status = iopstatus::kRun;
    }
    reschedule();
}

void IopKernel::dropThreadsAbove(u32 sequence) {
    for (IopThread& thread : mThreads) {
        if (thread.allocated && thread.sequence > sequence) {
            thread = {};
        }
    }
}

void IopKernel::reportThreads(const Symbols& symbols) const {
    for (usize i = 0; i < mThreads.size(); ++i) {
        const IopThread& thread = mThreads[i];
        if (thread.allocated && thread.status != iopstatus::kDormant) {
            log.info("thread {} id {:08x} {} at {} status {} priority {} waiting on {} {:08x} bits {:08x}",
                     i, makeHandle(i, thread.sequence), symbols.describe(thread.entry),
                     symbols.describe(i == mCurrent ? mIop.cpu.pc : thread.saved.pc),
                     thread.status, thread.currentPriority, getWaitName(thread.wait),
                     thread.waitId, thread.waitPattern);
        }
    }
}

bool IopKernel::isIdle() const {
    for (const IopThread& thread : mThreads) {
        if (thread.allocated && (thread.status & (iopstatus::kReady | iopstatus::kRun)) != 0) {
            return false;
        }
    }
    return true;
}

void IopKernel::exitCurrentThread() {
    mThreads[mCurrent].status = iopstatus::kDormant;
    reschedule();
}

void IopKernel::exitHostThread() {
    mThreads[kHostThread].status = iopstatus::kDormant;
    if (mCurrent == kHostThread) {
        reschedule();
    }
}

// Nothing here has a cache.
void IopKernel::flushCache() {
    returnValue(0);
}

// Refusing instead leaves MODLOAD asking forever.
void IopKernel::queryBootMode() {
    returnValue(0);
}

// LOADCORE: RegisterLibraryEntries @ 0x898
// Exports are read off the image as it loads.
void IopKernel::registerLibraryEntries() {
    returnValue(0);
}

// DMACMAN: sceSetDMAPriority. Nothing here arbitrates between channels.
void IopKernel::setDmaPriority() {
    returnValue(0);
}

// DMACMAN: sceEnableDMAChannel @ 0x114c
void IopKernel::enableDmaChannel() {
    returnValue(0);
}

// DMACMAN: dmacRequest @ 0xcc4
// It writes the channel registers, so a device sees the same transfer either way.
void IopKernel::requestDma() {
    const u32 channel = argument(0);
    if (channel >= kDmaChannels || channel == kUnusableDmaChannel) {
        returnValue(0);
        return;
    }
    const u32 direction = argument(4);
    const u32 base = getDmaChannelBase(channel);
    mMemory.write32(base + kDmaMadr, argument(1) & 0xFFFFFF);
    mMemory.write32(base + kDmaBcr, (argument(2) & 0xFFFF) | (argument(3) << 16));
    mMemory.write32(base + kDmaChcr,
                    (direction & 1) | kDmaSlice | (direction == 0 ? kDmaToMemory : 0));
    returnValue(1);
}

// DMACMAN: dmacStartTransfer @ 0xe84
void IopKernel::startDma() {
    const u32 channel = argument(0);
    if (channel >= kDmaChannels) {
        returnValue(0);
        return;
    }
    const u32 chcr = getDmaChannelBase(channel) + kDmaChcr;
    mMemory.write32(chcr, mMemory.read32(chcr) | kDmaStart);
    returnValue(1);
}

// LOADCORE: SetRebootTimeLibraryHandlingMode @ 0xb94
// Nothing here reboots.
void IopKernel::setRebootTimeLibraryHandlingMode() {
    returnValue(0);
}

// SECRMAN: SecrAuthCard @ 0xa4
// The challenge needs the console's own keys, which no modelled card holds.
void IopKernel::authCard() {
    returnValue(1);
}

// SECRMAN: SecrSetMcDevIDHandler @ 0x84
void IopKernel::setMcDevIdHandler() {
    returnValue(0);
}

// SECRMAN: SecrSetMcCommandHandler
// No memory card answers, so the handler would never run.
void IopKernel::setMcCommandHandler() {
    returnValue(0);
}

// SYSMEM: Kprintf @ 0xb90
void IopKernel::kprintf() {
    mConsole += format(argument(0), 1);
    returnValue(0);
}

// THREADMAN: CreateThread @ 0xc5c
void IopKernel::createThread() {
    const u32 param = argument(0);
    const u32 attr = mMemory.read32(param);
    const u32 entry = mMemory.read32(param + 8);
    const u32 stackSize = mMemory.read32(param + 12);
    const s32 priority = static_cast<s32>(mMemory.read32(param + 16));

    if ((attr & kThreadAttrMask) != 0) {
        returnValue(kIllegalAttr);
        return;
    }
    if (priority < 1 || priority > kLowestPriority) {
        returnValue(kIllegalPriority);
        return;
    }
    if ((entry & 3) != 0) {
        returnValue(kIllegalEntry);
        return;
    }
    if (stackSize < kSmallestStack) {
        returnValue(kIllegalStackSize);
        return;
    }

    usize index = 1;
    while (index < mThreads.size() && mThreads[index].allocated) {
        ++index;
    }
    if (index == mThreads.size()) {
        returnValue(kNoMemory);
        return;
    }

    const u32 rounded = (stackSize + kBlockAlignment - 1) & ~(kBlockAlignment - 1);
    const u32 stack = allocateBlock(kAllocLast, rounded, 0);
    if (stack == 0) {
        returnValue(kNoMemory);
        return;
    }

    IopThread& thread = mThreads[index];
    thread = {};
    thread.allocated = true;
    thread.sequence = ++mSequence;
    thread.status = iopstatus::kDormant;
    thread.attr = attr;
    thread.option = mMemory.read32(param + 4);
    thread.entry = entry;
    thread.stack = stack;
    thread.stackSize = rounded;
    thread.initialPriority = priority;
    thread.currentPriority = priority;
    returnValue(makeHandle(index, thread.sequence));
}

// THREADMAN: DeleteThread @ 0xe10
void IopKernel::deleteThread() {
    const u32 handle = argument(0);
    if (handle == 0) {
        returnValue(kIllegalThid);
        return;
    }
    IopThread* thread = threadFor(handle);
    if (thread == nullptr || handleIndex(handle) == mCurrent) {
        returnValue(kUnknownThid);
        return;
    }
    if (thread->status != iopstatus::kDormant) {
        returnValue(kNotDormant);
        return;
    }
    freeBlock(thread->stack);
    thread->allocated = false;
    returnValue(0);
}

// THREADMAN: StartThread @ 0x1028
void IopKernel::startThread() {
    const u32 handle = argument(0);
    if (handle == 0) {
        returnValue(kIllegalThid);
        return;
    }
    IopThread* thread = threadFor(handle);
    if (thread == nullptr) {
        returnValue(kUnknownThid);
        return;
    }
    if (thread->status != iopstatus::kDormant) {
        returnValue(kNotDormant);
        return;
    }

    thread->saved = R3000A{};
    thread->saved.pc = thread->entry;
    thread->saved.gpr[reg::a0] = argument(1);
    thread->saved.gpr[reg::sp] = thread->stack + thread->stackSize - 16;
    thread->saved.gpr[reg::gp] = mIop.cpu.gpr[reg::gp];
    thread->saved.gpr[reg::ra] = kThreadReturn;
    thread->status = iopstatus::kReady;
    returnValue(0);
    reschedule();
}

// THREADMAN: ExitThread @ 0x123c
void IopKernel::exitThread() {
    exitCurrentThread();
}

// THREADMAN: TerminateThread @ 0x12d8
void IopKernel::terminateThread() {
    const u32 handle = argument(0);
    if (handle == 0) {
        returnValue(kIllegalThid);
        return;
    }
    IopThread* thread = threadFor(handle);
    if (thread == nullptr) {
        returnValue(kUnknownThid);
        return;
    }
    thread->status = iopstatus::kDormant;
    thread->wait = IopWait::None;
    returnValue(0);
    reschedule();
}

// THREADMAN: GetThreadId @ 0x1ca8
void IopKernel::getThreadId() {
    returnValue(makeHandle(mCurrent, mThreads[mCurrent].sequence));
}

// THREADMAN: ReferThreadStatus @ 0x1f00
void IopKernel::referThreadStatus() {
    const u32 handle = argument(0);
    const u32 info = argument(1);
    IopThread* thread = handle == 0 ? &mThreads[mCurrent] : threadFor(handle);
    if (thread == nullptr) {
        returnValue(kUnknownThid);
        return;
    }
    mMemory.write32(info, thread->attr);
    mMemory.write32(info + 4, thread->option);
    mMemory.write32(info + 8, thread->status);
    mMemory.write32(info + 12, thread->entry);
    mMemory.write32(info + 16, thread->stack);
    mMemory.write32(info + 20, thread->stackSize);
    mMemory.write32(info + 24, 0);
    mMemory.write32(info + 28, static_cast<u32>(thread->initialPriority));
    mMemory.write32(info + 32, static_cast<u32>(thread->currentPriority));
    mMemory.write32(info + 36, 0);
    returnValue(0);
}

// THREADMAN: ChangeThreadPriority @ 0x1568
void IopKernel::changeThreadPriority() {
    const u32 handle = argument(0);
    const s32 priority = static_cast<s32>(argument(1));
    IopThread* thread = handle == 0 ? &mThreads[mCurrent] : threadFor(handle);
    if (thread == nullptr) {
        returnValue(kUnknownThid);
        return;
    }
    if (priority < 1 || priority > kLowestPriority) {
        returnValue(kIllegalPriority);
        return;
    }
    thread->currentPriority = priority;
    returnValue(0);
    reschedule();
}

// THREADMAN: SleepThread @ 0x200c
void IopKernel::sleepThread() {
    IopThread& thread = mThreads[mCurrent];
    if (thread.wakeupCount > 0) {
        --thread.wakeupCount;
        returnValue(0);
        return;
    }
    returnValue(0);
    blockCurrentThread(IopWait::Sleep, 0);
}

// THREADMAN: WakeupThread @ 0x20e4
void IopKernel::wakeupThread() {
    const u32 handle = argument(0);
    IopThread* thread = handle == 0 ? &mThreads[mCurrent] : threadFor(handle);
    if (thread == nullptr) {
        returnValue(kUnknownThid);
        return;
    }
    if (thread->status == iopstatus::kWait && thread->wait == IopWait::Sleep) {
        readyThread(handle == 0 ? mCurrent : handleIndex(handle));
        returnValue(0);
        reschedule();
        return;
    }
    ++thread->wakeupCount;
    returnValue(0);
}

// THREADMAN: CancelWakeupThread @ 0x22dc
void IopKernel::cancelWakeupThread() {
    const u32 handle = argument(0);
    IopThread* thread = handle == 0 ? &mThreads[mCurrent] : threadFor(handle);
    if (thread == nullptr) {
        returnValue(kUnknownThid);
        return;
    }
    const s32 banked = thread->wakeupCount;
    thread->wakeupCount = 0;
    returnValue(static_cast<u32>(banked));
}

// THREADMAN: DelayThread @ 0x2444
void IopKernel::delayThread() {
    const u64 cycles = static_cast<u64>(argument(0)) * kTicksPerMicrosecondNumerator /
                       kTicksPerMicrosecondDenominator;
    mThreads[mCurrent].waitDeadline = mCycles + cycles;
    setNextDeadline(mThreads[mCurrent].waitDeadline);
    blockCurrentThread(IopWait::Delay, 0);
    returnValue(0);
}

// THREADMAN: GetSystemTime @ 0x5738
void IopKernel::getSystemTime() {
    mMemory.write32(argument(0), static_cast<u32>(mCycles));
    mMemory.write32(argument(0) + 4, static_cast<u32>(mCycles >> 32));
    returnValue(0);
}

void IopKernel::usecToSysClock() {
    const u64 ticks = static_cast<u64>(argument(0)) * kTicksPerMicrosecondNumerator /
                      kTicksPerMicrosecondDenominator;
    mMemory.write32(argument(1), static_cast<u32>(ticks));
    mMemory.write32(argument(1) + 4, static_cast<u32>(ticks >> 32));
}

void IopKernel::sysClockToUsec() {
    const u32 clock = argument(0);
    const u64 ticks = mMemory.read32(clock) | (static_cast<u64>(mMemory.read32(clock + 4)) << 32);
    const u64 total = ticks * kTicksPerMicrosecondDenominator / kTicksPerMicrosecondNumerator;
    mMemory.write32(argument(1), static_cast<u32>(total / 1000000));
    mMemory.write32(argument(2), static_cast<u32>(total % 1000000));
}

// THREADMAN: CreateSema @ 0x3060
void IopKernel::createSema() {
    const u32 param = argument(0);
    const u32 attr = mMemory.read32(param);
    if ((attr & kSemaAttrMask) != 0) {
        returnValue(kIllegalAttr);
        return;
    }
    usize index = 0;
    while (index < mSemaphores.size() && mSemaphores[index].allocated) {
        ++index;
    }
    if (index == mSemaphores.size()) {
        returnValue(kNoMemory);
        return;
    }
    IopSemaphore& semaphore = mSemaphores[index];
    semaphore = {};
    semaphore.allocated = true;
    semaphore.sequence = ++mSequence;
    semaphore.attr = attr;
    semaphore.option = mMemory.read32(param + 4);
    semaphore.initial = static_cast<s32>(mMemory.read32(param + 8));
    semaphore.count = semaphore.initial;
    semaphore.maxCount = static_cast<s32>(mMemory.read32(param + 12));
    returnValue(makeHandle(index, semaphore.sequence));
}

// THREADMAN: DeleteSema @ 0x3164
void IopKernel::deleteSema() {
    const u32 handle = argument(0);
    IopSemaphore* semaphore = semaphoreFor(handle);
    if (semaphore == nullptr) {
        returnValue(kUnknownSemaid);
        return;
    }
    for (usize i = 0; i < mThreads.size(); ++i) {
        if (mThreads[i].status == iopstatus::kWait && mThreads[i].wait == IopWait::Semaphore &&
            mThreads[i].waitId == handle) {
            readyThread(i);
        }
    }
    semaphore->allocated = false;
    returnValue(0);
    reschedule();
}

// THREADMAN: SignalSema @ 0x328c
void IopKernel::signalSema() {
    const u32 handle = argument(0);
    IopSemaphore* semaphore = semaphoreFor(handle);
    if (semaphore == nullptr) {
        returnValue(kUnknownSemaid);
        return;
    }
    for (usize i = 0; i < mThreads.size(); ++i) {
        if (mThreads[i].status == iopstatus::kWait && mThreads[i].wait == IopWait::Semaphore &&
            mThreads[i].waitId == handle) {
            readyThread(i);
            returnValue(0);
            reschedule();
            return;
        }
    }
    if (semaphore->count >= semaphore->maxCount) {
        returnValue(kSemaOverflow);
        return;
    }
    ++semaphore->count;
    returnValue(0);
}

// THREADMAN: WaitSema @ 0x3444
void IopKernel::waitSema() {
    const u32 handle = argument(0);
    IopSemaphore* semaphore = semaphoreFor(handle);
    if (semaphore == nullptr) {
        returnValue(kUnknownSemaid);
        return;
    }
    if (semaphore->count > 0) {
        --semaphore->count;
        returnValue(0);
        return;
    }
    returnValue(0);
    blockCurrentThread(IopWait::Semaphore, handle);
}

// THREADMAN: PollSema @ 0x35b4
void IopKernel::pollSema() {
    const u32 handle = argument(0);
    IopSemaphore* semaphore = semaphoreFor(handle);
    if (semaphore == nullptr) {
        returnValue(kUnknownSemaid);
        return;
    }
    if (semaphore->count <= 0) {
        returnValue(kSemaZero);
        return;
    }
    --semaphore->count;
    returnValue(0);
}

void IopKernel::referSemaStatus() {
    const u32 handle = argument(0);
    const u32 info = argument(1);
    IopSemaphore* semaphore = semaphoreFor(handle);
    if (semaphore == nullptr) {
        returnValue(kUnknownSemaid);
        return;
    }
    mMemory.write32(info, semaphore->attr);
    mMemory.write32(info + 4, semaphore->option);
    mMemory.write32(info + 8, static_cast<u32>(semaphore->initial));
    mMemory.write32(info + 12, static_cast<u32>(semaphore->maxCount));
    mMemory.write32(info + 16, static_cast<u32>(semaphore->count));
    mMemory.write32(info + 20, waitingOn(IopWait::Semaphore, handle));
    returnValue(0);
}

// THREADMAN: CreateEventFlag @ 0x2560
void IopKernel::createEventFlag() {
    const u32 param = argument(0);
    usize index = 0;
    while (index < mEventFlags.size() && mEventFlags[index].allocated) {
        ++index;
    }
    if (index == mEventFlags.size()) {
        returnValue(kNoMemory);
        return;
    }
    IopEventFlag& flag = mEventFlags[index];
    flag = {};
    flag.allocated = true;
    flag.sequence = ++mSequence;
    flag.attr = mMemory.read32(param);
    flag.option = mMemory.read32(param + 4);
    flag.initialBits = mMemory.read32(param + 8);
    flag.bits = flag.initialBits;
    returnValue(makeHandle(index, flag.sequence));
}

// THREADMAN: DeleteEventFlag @ 0x2660
void IopKernel::deleteEventFlag() {
    IopEventFlag* flag = eventFlagFor(argument(0));
    if (flag == nullptr) {
        returnValue(kUnknownEventFlagId);
        return;
    }
    flag->allocated = false;
    returnValue(0);
}

// THREADMAN: SetEventFlag @ 0x2788
void IopKernel::setEventFlag() {
    const u32 handle = argument(0);
    IopEventFlag* flag = eventFlagFor(handle);
    if (flag == nullptr) {
        returnValue(kUnknownEventFlagId);
        return;
    }
    flag->bits |= argument(1);
    for (usize i = 0; i < mThreads.size(); ++i) {
        IopThread& thread = mThreads[i];
        if (thread.status != iopstatus::kWait || thread.wait != IopWait::EventFlag ||
            thread.waitId != handle) {
            continue;
        }
        if (eventFlagSatisfied(flag->bits, thread.waitPattern, thread.waitMode)) {
            if (thread.resultAddress != 0) {
                mMemory.write32(thread.resultAddress, flag->bits);
            }
            if ((thread.waitMode & kEventFlagClear) != 0) {
                flag->bits = 0;
            }
            readyThread(i);
        }
    }
    returnValue(0);
    reschedule();
}

// THREADMAN: ClearEventFlag @ 0x2abc
void IopKernel::clearEventFlag() {
    IopEventFlag* flag = eventFlagFor(argument(0));
    if (flag == nullptr) {
        returnValue(kUnknownEventFlagId);
        return;
    }
    flag->bits &= argument(1);
    returnValue(0);
}

// THREADMAN: WaitEventFlag @ 0x2bd0
void IopKernel::waitEventFlag() {
    const u32 handle = argument(0);
    const u32 pattern = argument(1);
    const u32 mode = argument(2);
    const u32 result = argument(3);
    if ((mode & kEventFlagModeMask) != 0) {
        returnValue(kIllegalMode);
        return;
    }
    if (pattern == 0) {
        returnValue(kEventFlagPattern);
        return;
    }
    IopEventFlag* flag = eventFlagFor(handle);
    if (flag == nullptr) {
        returnValue(kUnknownEventFlagId);
        return;
    }
    if (eventFlagSatisfied(flag->bits, pattern, mode)) {
        if (result != 0) {
            mMemory.write32(result, flag->bits);
        }
        if ((mode & kEventFlagClear) != 0) {
            flag->bits = 0;
        }
        returnValue(0);
        return;
    }
    IopThread& thread = mThreads[mCurrent];
    thread.waitPattern = pattern;
    thread.waitMode = mode;
    thread.resultAddress = result;
    returnValue(0);
    blockCurrentThread(IopWait::EventFlag, handle);
}

// THREADMAN: PollEventFlag @ 0x2db0
void IopKernel::pollEventFlag() {
    const u32 handle = argument(0);
    const u32 pattern = argument(1);
    const u32 mode = argument(2);
    const u32 result = argument(3);
    if ((mode & kEventFlagModeMask) != 0) {
        returnValue(kIllegalMode);
        return;
    }
    if (pattern == 0) {
        returnValue(kEventFlagPattern);
        return;
    }
    IopEventFlag* flag = eventFlagFor(handle);
    if (flag == nullptr) {
        returnValue(kUnknownEventFlagId);
        return;
    }
    if (!eventFlagSatisfied(flag->bits, pattern, mode)) {
        returnValue(kEventFlagCondition);
        return;
    }
    if (result != 0) {
        mMemory.write32(result, flag->bits);
    }
    if ((mode & kEventFlagClear) != 0) {
        flag->bits = 0;
    }
    returnValue(0);
}

// THREADMAN: ReferEventFlagStatus @ 0x2f4c
void IopKernel::referEventFlagStatus() {
    const u32 handle = argument(0);
    const u32 info = argument(1);
    IopEventFlag* flag = eventFlagFor(handle);
    if (flag == nullptr) {
        returnValue(kUnknownEventFlagId);
        return;
    }
    mMemory.write32(info, flag->attr);
    mMemory.write32(info + 4, flag->option);
    mMemory.write32(info + 8, flag->initialBits);
    mMemory.write32(info + 12, flag->bits);
    mMemory.write32(info + 16, waitingOn(IopWait::EventFlag, handle));
    returnValue(0);
}

// SIFMAN: sceSifInit @ 0x148
void IopKernel::sifInit() {
    returnValue(0);
}

// SIFMAN (310): sifman_SifSetDma @ 0x95c
// PADMAN md5 0fa9dbfe8794e172733189ed945aff45: PadSifDmaSend @ 0x360
void IopKernel::runSifTransfers(u32 transfers, u32 count) {
    for (u32 i = 0; i < count; ++i) {
        const u32 entry = transfers + i * kSifTransferSize;
        const u32 source = mMemory.read32(entry);
        const u32 destination = mMemory.read32(entry + 4);
        const u32 size = mMemory.read32(entry + 8);
        for (u32 offset = 0; offset < size; ++offset) {
            mMainMemory.write8(destination + offset, mMemory.read8(source + offset));
        }
        log.trace("sif dma {:08x} to {:08x}, {} bytes", source, destination, size);
    }
}

void IopKernel::sifSetDma() {
    const u32 count = argument(1);
    runSifTransfers(argument(0), count);
    returnValue(count);
}

// SIFCMD: SifGetOtherData @ 0xad4
// The bytes are in place on return; the module leaves them to be polled.
void IopKernel::sifGetOtherData() {
    const u32 source = argument(1);
    const u32 destination = argument(2);
    const u32 size = argument(3);
    for (u32 offset = 0; offset < size; ++offset) {
        mMemory.write8(destination + offset, mMainMemory.read8(source + offset));
    }
    log.trace("sif read {:08x} to {:08x}, {} bytes", source, destination, size);
    returnValue(0);
}

// DBCMAN: dbcman_e8 @ 0x31c, imported as sifman ordinal 32
// The handler falls due as soon as the bytes move.
void IopKernel::sifSetDmaIntr() {
    const u32 count = argument(1);
    runSifTransfers(argument(0), count);
    mPendingHandlers.push_back({argument(2), argument(3), mIop.cpu.gpr[reg::gp]});
    returnValue(count);
}

// SIFMAN: sceSifCheckInit @ 0x2d8
void IopKernel::sifCheckInit() {
    returnValue(1);
}

// SIFCMD: sceSifInitRpc @ 0x6c0
void IopKernel::sifInitRpc() {
    returnValue(0);
}

// SIFMAN: sceSifDmaStat @ 0x740
// A transfer finishes the moment it is asked for.
void IopKernel::sifDmaStat() {
    returnValue(kFailed);
}

// SIFCMD: sceSifBindRpc @ 0xd08
// Nothing carries a request that way yet.
void IopKernel::sifBindRpc() {
    returnValue(0);
    blockCurrentThread(IopWait::RpcReply, argument(0));
}

// SIFCMD: sceSifCallRpc @ 0xed4. Nothing here answers a call the other way.
void IopKernel::sifCallRpc() {
    returnValue(kFailed);
}

// SIFCMD: sceSifSetRpcQueue @ 0x1088
void IopKernel::sifSetRpcQueue() {
    mMemory.write32(argument(0) + kQueueThreadId, argument(1));
    returnValue(argument(0));
}

// SIFCMD: sceSifRegisterRpc @ 0x1130
// The EE reaches the handler through Sif, not a request packet.
void IopKernel::sifRegisterRpc() {
    const u32 data = argument(0);
    const u32 id = argument(1);
    const u32 handler = argument(2);
    const u32 buffer = argument(3);
    mMemory.write32(data + kServerId, id);
    mMemory.write32(data + kServerFunc, handler);
    mMemory.write32(data + kServerBuffer, buffer);
    mMemory.write32(data + kServerClientFunc, argument(4));
    mMemory.write32(data + kServerClientBuffer, argument(5));
    mRpcServers.push_back({id, data, handler, buffer, mIop.cpu.gpr[kGlobalRegister]});
    returnValue(data);
}

// SIFCMD: sceSifRpcLoop @ 0x1520. Requests arrive as host calls instead, so the
// thread that would serve them has nothing left to do.
void IopKernel::sifRpcLoop() {
    blockCurrentThread(IopWait::RpcQueue, argument(0));
}

// SIFCMD: sceSifRemoveRpc @ 0x1210
void IopKernel::sifRemoveRpc() {
    const u32 id = mMemory.read32(argument(0) + kServerId);
    std::erase_if(mRpcServers, [id](const IopRpcServer& server) { return server.id == id; });
    returnValue(0);
}

// SIFCMD: sceSifRemoveRpcQueue @ 0x12a8
void IopKernel::sifRemoveRpcQueue() {
    returnValue(argument(0));
}

// VBLANK: WaitVblankEnd @ 0x574
void IopKernel::waitVblankEnd() {
    returnValue(0);
    blockCurrentThread(IopWait::Vblank, 0);
}

u32 IopKernel::allocateMemory(u32 size) {
    return allocateBlock(kAllocFirst, size, 0);
}

u32 IopKernel::allocateMemory(u32 mode, u32 size, u32 address) {
    return allocateBlock(mode, size, address);
}

bool IopKernel::freeMemory(u32 address) {
    return freeBlock(address);
}

std::vector<IopKernel::VblankHandler>& IopKernel::vblankHandlersFor(bool atEnd) {
    return atEnd ? mVblankEndHandlers : mVblankStartHandlers;
}

// VBLANK: RegisterVblankHandler @ 0x164
// VBLANK: VblankStartIntrHandler @ 0x374
// Lower priority runs earlier.
void IopKernel::registerVblankHandler() {
    const bool atEnd = argument(0) != 0;
    const s32 priority = static_cast<s32>(argument(1));
    const u32 handler = argument(2);
    std::vector<VblankHandler>& handlers = vblankHandlersFor(atEnd);
    for (const VblankHandler& registered : handlers) {
        if (registered.handler == handler) {
            returnValue(kFoundHandler);
            return;
        }
    }
    const auto after = std::find_if(handlers.begin(), handlers.end(),
                                    [priority](const VblankHandler& other) {
                                        return priority < other.priority;
                                    });
    handlers.insert(after, {handler, argument(3), mIop.cpu.gpr[reg::gp], priority});
    log.trace("vblank {} handler {:08x} priority {}", atEnd ? "end" : "start", handler, priority);
    returnValue(0);
}

// VBLANK: ReleaseVblankHandler @ 0x2ac
void IopKernel::releaseVblankHandler() {
    const u32 handler = argument(1);
    std::vector<VblankHandler>& handlers = vblankHandlersFor(argument(0) != 0);
    const usize removed = std::erase_if(handlers, [handler](const VblankHandler& registered) {
        return registered.handler == handler;
    });
    returnValue(removed != 0 ? 0 : kNotFoundHandler);
}

// What the clock alone will release: a thread whose delay expires, or a device
// whose transfer finishes. A run with neither has nothing left to wake it.
u64 IopKernel::findNextDeadline() const {
    u64 next = kNoDeadline;
    for (const IopThread& thread : mThreads) {
        if (thread.allocated && thread.status == iopstatus::kWait &&
            thread.wait == IopWait::Delay) {
            next = std::min(next, thread.waitDeadline);
        }
    }
    for (const auto& [deadline, line] : mDeferredInterrupts) {
        next = std::min(next, deadline);
    }
    for (const IopHardTimer& timer : mHardTimers) {
        if (timer.running && timer.handler != 0 && timer.compare != 0) {
            next = std::min(next, timer.nextFire);
        }
    }
    return next;
}

void IopKernel::setNextDeadline(u64 at) {
    if (mNextDeadline != 0) {
        mNextDeadline = std::min(mNextDeadline, at);
    }
}

std::optional<u64> IopKernel::nextDelay() const {
    std::optional<u64> earliest;
    for (const IopThread& thread : mThreads) {
        if (thread.allocated && thread.status == iopstatus::kWait &&
            thread.wait == IopWait::Delay) {
            earliest = std::min(earliest.value_or(thread.waitDeadline), thread.waitDeadline);
        }
    }
    for (const auto& [deadline, line] : mDeferredInterrupts) {
        earliest = std::min(earliest.value_or(deadline), deadline);
    }
    return earliest;
}

void IopKernel::advanceTimeTo(u64 cycles) {
    advanceTime(cycles > mCycles ? cycles - mCycles : 0);
}

// The devices keep this clock, including the stretches the scheduler skips
// while every thread waits.
void IopKernel::advanceTime(u64 instructions) {
    mCycles += instructions;
    if (mDevices != nullptr) {
        mDevices->advance(instructions);
    }
    if (mNextDeadline != 0 && mCycles < mNextDeadline) {
        return;
    }
    for (usize i = mDeferredInterrupts.size(); i > 0; --i) {
        const auto& [deadline, line] = mDeferredInterrupts[i - 1];
        if (mCycles >= deadline) {
            raiseInterrupt(line);
            mDeferredInterrupts.erase(mDeferredInterrupts.begin() + static_cast<isize>(i) - 1);
        }
    }
    for (IopHardTimer& timer : mHardTimers) {
        if (!timer.running || timer.handler == 0 || timer.compare == 0 ||
            mCycles < timer.nextFire) {
            continue;
        }
        timer.nextFire += timer.compare;
        mPendingHandlers.push_back({timer.handler, timer.argument, timer.gp});
    }

    bool woke = false;
    for (usize i = 0; i < mThreads.size(); ++i) {
        const IopThread& thread = mThreads[i];
        if (thread.allocated && thread.status == iopstatus::kWait &&
            thread.wait == IopWait::Delay && mCycles >= thread.waitDeadline) {
            readyThread(i);
            woke = true;
        }
    }
    // Every thread was blocked, so there is no current one to hand back to.
    if (woke) {
        reschedule();
    }
    mNextDeadline = findNextDeadline();
}

void IopKernel::queueHandlers(const std::vector<VblankHandler>& handlers) {
    for (const VblankHandler& registered : handlers) {
        mPendingHandlers.push_back({registered.handler, registered.argument, registered.gp});
    }
}

void IopKernel::raiseVblankStart() {
    queueHandlers(mVblankStartHandlers);
}

void IopKernel::raiseVblankEnd() {
    queueHandlers(mVblankEndHandlers);
    for (usize i = 0; i < mThreads.size(); ++i) {
        const IopThread& thread = mThreads[i];
        if (thread.allocated && thread.status == iopstatus::kWait &&
            thread.wait == IopWait::Vblank) {
            readyThread(i);
        }
    }
}

// TIMEMANI: AllocHardTimer @ 0x90
void IopKernel::allocHardTimer() {
    for (u32 i = 0; i < kHardTimers; ++i) {
        if (!mHardTimers[i].allocated) {
            mHardTimers[i] = {};
            mHardTimers[i].allocated = true;
            returnValue((i + 1) << 28);
            return;
        }
    }
    returnValue(0);
}

IopHardTimer* IopKernel::hardTimerFor(u32 timid) {
    const u32 index = (timid >> 28) - 1;
    if (index >= kHardTimers || !mHardTimers[index].allocated) {
        return nullptr;
    }
    return &mHardTimers[index];
}

// TIMEMANI: FreeHardTimer @ 0x3b0
void IopKernel::freeHardTimer() {
    IopHardTimer* timer = hardTimerFor(argument(0));
    if (timer != nullptr) {
        *timer = {};
    }
    returnValue(0);
}

// TIMEMANI: SetTimerHandler @ 0x6f0. The handler is a function in the module
// that registers it, so it needs that module's gp to reach its own data.
void IopKernel::setTimerHandler() {
    IopHardTimer* timer = hardTimerFor(argument(0));
    if (timer == nullptr) {
        returnValue(kIllegalIntrCode);
        return;
    }
    timer->compare = argument(1);
    timer->handler = argument(2);
    timer->argument = argument(3);
    timer->gp = mIop.cpu.gpr[reg::gp];
    returnValue(0);
}

void IopKernel::setupHardTimer() {
    returnValue(hardTimerFor(argument(0)) == nullptr ? kIllegalIntrCode : 0);
}

void IopKernel::startHardTimer() {
    IopHardTimer* timer = hardTimerFor(argument(0));
    if (timer != nullptr) {
        timer->running = true;
        timer->nextFire = mCycles + timer->compare;
        setNextDeadline(timer->nextFire);
    }
    returnValue(0);
}

// INTRMANI: IntrDispatch @ 0xe08 runs the handler a line was registered with.
// A line nothing claimed is not an error; the hardware raises it regardless.
void IopKernel::raiseInterrupt(u32 line) {
    if (line >= std::size(mInterrupts) || mInterrupts[line].handler == 0) {
        return;
    }
    const Interrupt& claimed = mInterrupts[line];
    mPendingHandlers.push_back({claimed.handler, claimed.argument, claimed.gp});
}

// Hardware finishes a transfer when the data has moved, not when the register
// is written, and a driver that refills on the interrupt runs at that pace.
void IopKernel::raiseInterruptAfter(u32 line, u64 cycles) {
    mDeferredInterrupts.emplace_back(mCycles + cycles, line);
    setNextDeadline(mCycles + cycles);
}

std::vector<IopHandlerCall> IopKernel::takePendingHandlers() {
    return std::exchange(mPendingHandlers, {});
}

void IopKernel::stopHardTimer() {
    IopHardTimer* timer = hardTimerFor(argument(0));
    if (timer != nullptr) {
        timer->running = false;
    }
    returnValue(0);
}

// LIBSD: sceSdInit @ 0x1428. There is no SPU2, so the calls below take what a
// caller writes and report that the hardware accepted it.
void IopKernel::sdInit() {
    returnValue(0);
}

void IopKernel::sdAccepted() {
    returnValue(0);
}

// LIBSD: sceSdSetParam @ 0x3160
void IopKernel::sdSetParam() {
    mSdParams[argument(0)] = argument(1);
    returnValue(0);
}

// LIBSD: sceSdGetParam @ 0x31bc
void IopKernel::sdGetParam() {
    returnValue(readRegister(mSdParams, argument(0)));
}

// LIBSD: sceSdSetSwitch @ 0x3a5c
void IopKernel::sdSetSwitch() {
    mSdSwitches[argument(0)] = argument(1);
    returnValue(0);
}

// LIBSD: sceSdGetSwitch @ 0x3a94
void IopKernel::sdGetSwitch() {
    returnValue(readRegister(mSdSwitches, argument(0)));
}

// LIBSD: sceSdSetAddr @ 0x3acc
void IopKernel::sdSetAddr() {
    mSdAddresses[argument(0)] = argument(1);
    returnValue(0);
}

// LIBSD: sceSdGetAddr @ 0x3b30
void IopKernel::sdGetAddr() {
    returnValue(readRegister(mSdAddresses, argument(0)));
}

// LIBSD: sceSdSetCoreAttr @ 0x3fec
void IopKernel::sdSetCoreAttr() {
    mSdCoreAttrs[argument(0)] = argument(1);
    returnValue(argument(1));
}

// LIBSD: sceSdGetCoreAttr @ 0x40a0
void IopKernel::sdGetCoreAttr() {
    returnValue(readRegister(mSdCoreAttrs, argument(0)));
}

// LIBSD: sceSdNote2Pitch @ 0x3bd0 and sceSdPitch2Note @ 0x3ce4. Nothing here
// plays a note, and no caller branches on the answer.
void IopKernel::sdConvertPitch() {
    returnValue(0);
}

// LIBSD: sceSdProcBatch @ 0x33fc and sceSdProcBatchEx @ 0x35e8, which report
// how many of the batch they applied.
void IopKernel::sdProcBatch() {
    returnValue(argument(1));
}

// LIBSD: sceSdSetEffectAttr @ 0xc2c
void IopKernel::sdSetEffectAttr() {
    returnValue(0);
}

// LIBSD: sceSdGetEffectAttr @ 0xbbc
void IopKernel::sdGetEffectAttr() {
    returnValue(0);
}

// LIBSD: sceSdClearEffectWorkArea @ 0x660
void IopKernel::sdClearEffectWorkArea() {
    returnValue(0);
}

// LIBSD: sceSdSetSpu2IntrHandler @ 0x41e0
void IopKernel::sdSetSpu2IntrHandler() {
    const u32 previous = mSpu2IntrHandler;
    mSpu2IntrHandler = argument(0);
    mSpu2IntrArgument = argument(1);
    returnValue(previous);
}

// LIBSD: sceSdVoiceTrans @ 0x14f0
void IopKernel::sdVoiceTrans() {
    returnValue(argument(3));
}

// LIBSD: sceSdBlockTrans @ 0x1710
void IopKernel::sdBlockTrans() {
    returnValue(argument(3));
}

// LIBSD: sceSdVoiceTransStatus @ 0x1620 and sceSdBlockTransStatus @ 0x1890.
// Zero means the transfer is still running, which nothing here would ever end.
void IopKernel::sdTransStatus() {
    returnValue(1);
}

// LIBSD: sceSdSetTransIntrHandler @ 0x38e8
void IopKernel::sdSetTransIntrHandler() {
    const u32 previous = mTransIntrHandler;
    mTransIntrHandler = argument(1);
    mTransIntrArgument = argument(2);
    returnValue(previous);
}

// LIBSD: ordinal 28, which this build does not name.
void IopKernel::sdGetTransIntrHandlerArgument() {
    returnValue(mTransIntrArgument);
}

}  // namespace uee
