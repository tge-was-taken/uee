// A `KERNEL: <Name> @ <address>` tag names the routine reimplemented below:
//
//   KERNEL.elf  94088 bytes  md5 d3365049f363abad59102ec4d4e2b0af
//               .text 0x80000000..0x80016e28
//   EELOAD.elf  62192 bytes  md5 54ccf6ebf95a042d3560b843c7e5dc85
//               .text 0x00082000..0x00091190
//
// Neither carries a version string; the closest marker is
// "EE DECI2 Manager version 0.06" at 0x800164e0 in KERNEL.elf.

#include "log.h"
#include "kernel.h"

#include "sif.h"
#include "memory.h"
#include "syscall_table.h"

#include <cstdio>
#include <cstdlib>

#include <format>
#include <utility>

namespace uee {
namespace {

LoggerChannel log("kernel");

constexpr u32 kStackPointer = 29;
constexpr u32 kGlobalPointer = 28;
constexpr u32 kReturnAddress = 31;
constexpr u32 kFirstArgument = 4;
constexpr u32 kReturnRegister = 2;
constexpr u32 kSyscallRegister = 3;

// Status.IM2 gates INTC, IM3 the DMAC, bit 16 the master enable ei/di drive.
constexpr u32 kStatusIntc = 1u << 10;
constexpr u32 kStatusDmac = 1u << 11;
constexpr u32 kStatusMasterEnable = 1u << 16;
// The rom leaves the three coprocessors usable and the exception handler's own
// interrupt disable armed.
constexpr u32 kStatusCoprocessors = 0x70000000;
constexpr u32 kStatusExceptionDisable = 1u << 17;
// The counter has been running since power-on, so it has long since passed the
// compare register the rom left at zero.
constexpr u32 kCauseProbeRemains = 0x70000000 | cop0::kCounterInterrupt;
constexpr u32 kMachinePsMode = 0x8000;
// KERNEL: SifSetDChain @ 0x8000c8b0 answers with the SIF0 channel control it left.
constexpr u32 kSifDChainResult = 0x184;

// A queued call that never runs stops every interrupt behind it.
constexpr u32 kHeldCallsReported = 100000;
constexpr u32 kGsImrRegister = 0x12001010;
constexpr s32 kDeci2Call = 124;
// KERNEL: Deci2CallHandler @ 0x8000ecc8 sends this one to KttyCall.
constexpr u32 kKernelTty = 0x10;
constexpr u32 kTtyLimit = 4096;
constexpr u32 kSif0Chcr = 0x1000C000;
constexpr u32 kSif0Qwc = 0x1000C020;

// COP0 Config bits CpuConfig switches, and the K0 cache mode KSeg0 rewrites.
constexpr u32 kConfigBpe = 0x00040000;
constexpr u32 kConfigIce = 0x00002000;
constexpr u32 kConfigDie = 0x00001000;
constexpr u32 kConfigKernelSegmentMask = 0x7;
constexpr u32 kCop0Config = 16;

constexpr u32 kThreadFunction = 4;
constexpr u32 kThreadStack = 8;
constexpr u32 kThreadStackSize = 12;
constexpr u32 kThreadGlobalPointer = 16;
constexpr u32 kThreadPriority = 20;
constexpr u32 kThreadAttr = 28;
constexpr u32 kThreadOption = 32;

constexpr u32 kSemaphoreMaxCount = 4;
constexpr u32 kSemaphoreInitialCount = 8;
constexpr u32 kSemaphoreAttr = 16;
constexpr u32 kSemaphoreOption = 20;

constexpr u32 kUnusedStack = 0xFFFFFFFFu;
constexpr u32 kRestOfMemory = 0xFFFFFFFFu;



// Nothing dereferences it; the guest refuses to call an unbound server.


// SifSetReg/SifGetReg reach SBUS by these ids, everything else by a 32 slot file.
constexpr u32 kMainToSubCommand = 1;
constexpr u32 kSubToMainCommand = 2;
constexpr u32 kMainToSubFlag = 3;
constexpr u32 kSubToMainFlag = 4;
constexpr u32 kSoftwareRegister = 0x80000000;
constexpr u32 kSoftwareRegisters = 0x20;

}  // namespace

Kernel::Kernel(EeContext& ctx, Memory& memory, const GameConfig& config,
               std::span<TranslatedIrx* const> iopModules)
    : mContext(ctx), mMemory(memory), mWatched(config.watch),
      mIopBudgetPerHsync(config.iopClockScale == 0
                             ? ~u64{0} / 2
                             : kIopInstructionsPerHsync * config.iopClockScale),
      mIop(memory, mOutput, iopModules, config.hostModules, mWatched) {
    mMemory.attach(&mMmio);
    mMmio.attach(mMemory, mContext);
    installSyscallTable(mMemory);

    // As the ROM's boot leaves it: taking INTC and DMAC interrupts.
    mContext.cpu.cop0[cop0::kStatus] = cop0::kInterruptEnable | kStatusIntc | kStatusDmac |
                                       kStatusMasterEnable | kStatusCoprocessors |
                                       kStatusExceptionDisable;
    mContext.cpu.cop0[cop0::kProcessorId] = cop0::kR5900Id;
    mContext.cpu.cop0[cop0::kConfig] = cop0::kR5900Config;
    mContext.cpu.cop0[cop0::kWired] = cop0::kWiredAfterBoot;
    // What the rom's own probe of the coprocessor it has no unit for leaves in
    // the exception code's neighbours.
    mContext.cpu.cop0[cop0::kCause] = kCauseProbeRemains;
    for (usize i = 0; i < cop0::kTlbEntries; ++i) {
        mContext.cpu.tlb[i] = cop0::kBootTlb[i];
    }
    mContext.cpu.cop0[cop0::kIndex] = cop0::kBootIndex;
    mContext.cpu.cop0[cop0::kRandom] = cop0::kBootRandom;
    mContext.cpu.cop0[cop0::kContext] = cop0::kBootContext;
    mContext.cpu.cop0[cop0::kBadVAddr] = cop0::kBootBadVAddr;
    mContext.cpu.cop0[cop0::kEntryHi] = cop0::kBootEntryHi;
    mContext.cpu.cop0[cop0::kCompare] = cop0::kBootCompare;
    mContext.cpu.cop0[cop0::kEpc] = cop0::kBootEpc;
    mContext.cpu.cop0[cop0::kBadPAddr] = cop0::kBootBadPAddr;
    mContext.cpu.performanceControl = cop0::kBootPerformance;
    mContext.cpu.cop0[27] = cop0::kBootReserved27;
    mContext.cpu.cop0[cop0::kTagLo] = cop0::kBootTagLo;
    mContext.cpu.cop0[cop0::kTagHi] = cop0::kBootTagHi;
    mContext.cpu.cop0[cop0::kErrorEpc] = cop0::kBootErrorEpc;

    // KERNEL: InitThreads @ 0x80004ec0
    // Its CreateRomModuleThread("EENULL") takes the first block, so guest ids
    // start at 1, and pthreads-embedded reads zero as no thread at all.
    mThreads.at(static_cast<usize>(mThreads.allocate())).status = status::kDormant;

    const s32 main = mThreads.allocate();
    mThreads.at(static_cast<usize>(main)).currentPriority = 0;
    mThreads.at(static_cast<usize>(main)).initialPriority = 0;
    mThreads.makeReady(static_cast<usize>(main));
    mThreads.at(static_cast<usize>(main)).status = status::kRun;
    mThreads.setCurrent(static_cast<usize>(main));

}

u32 Kernel::readArgument(u32 index) const {
    return static_cast<u32>(mContext.cpu.gpr[kFirstArgument + index].lo);
}

void Kernel::returnValue(u32 value) {
    mContext.cpu.write32(kReturnRegister, value);
}

// KERNEL: SyscallHandler @ 0x80000280
SyscallResult Kernel::callSyscall(s32 number) {
    mContext.cpu.write64(kSyscallRegister, static_cast<u64>(static_cast<s64>(number)));
    return handleSyscall();
}

SyscallResult Kernel::handleSyscall() {
    // The handler is taken over rather than run, so the cause it would have left
    // is written here. EPC is not, since no exception was taken.
    u32& cause = mContext.cpu.cop0[cop0::kCause];
    cause = (cause & ~cop0::kCauseCodeMask) | (cop0::kSystemCall << cop0::kCauseCodeShift);
    const s32 raw = static_cast<s32>(static_cast<u32>(mContext.cpu.gpr[kSyscallRegister].lo));
    // The sign selects interrupt context, which must not reschedule.
    const s32 number = raw < 0 ? -raw : raw;
    const bool interruptContext = raw < 0 || isInterruptSyscall(number);

    // A positive 124 jumps to the DECI2 manager without indexing the table.
    if (!interruptContext && number == kDeci2Call) {
        const s32 requested = static_cast<s32>(readArgument(0));
        // A negative number skips the debug save and takes its block from $a2.
        returnValue(static_cast<u32>(
            requested < 0 ? deci2Call(static_cast<u32>(-requested), readArgument(2))
                          : deci2Call(static_cast<u32>(requested), readArgument(1))));
        return SyscallResult::Handled;
    }

    log.trace("syscall {} {} a0={:08x} a1={:08x} thread {} status {} wait {} wakeups {} ra={:08x}",
              number, nameOfSyscall(number), readArgument(0), readArgument(1), mThreads.current(),
              mThreads.at(mThreads.current()).status, mThreads.at(mThreads.current()).waitType,
              mThreads.at(mThreads.current()).wakeupCount,
              static_cast<u32>(mContext.cpu.gpr[kReturnAddress].lo));

    const u32 handler = readSyscallHandler(mMemory, number);
    if (!isKernelHandler(handler)) {
        if (handler == 0) {
            return SyscallResult::Unknown;
        }
        // It returns with jr $ra, so call it rather than enter it as an exception.
        mSyscallReturn = SyscallReturn{static_cast<u32>(mContext.cpu.gpr[kReturnAddress].lo),
                                       mContext.cpu.pc};
        mContext.cpu.write64(kReturnAddress, kSyscallHandlerReturn);
        mContext.cpu.pc = handler;
        return SyscallResult::Handled;
    }
    return dispatch(handler, interruptContext);
}

SyscallResult Kernel::dispatch(u32 handler, bool interruptContext) {
    const auto arg = [&](u32 index) { return readArgument(index); };
    const auto signed0 = [&] { return static_cast<s32>(readArgument(0)); };

    switch (static_cast<Syscall>(handler)) {
        case Syscall::Exit:
            return SyscallResult::Exited;

        case Syscall::SetupThread:
            returnValue(setupThread(arg(0), arg(1), arg(2), arg(4)));
            return SyscallResult::Handled;

        case Syscall::SetupHeap:
            returnValue(setupHeap(arg(0), arg(1)));
            return SyscallResult::Handled;

        case Syscall::EndOfHeap:
            returnValue(mHeapEnd);
            return SyscallResult::Handled;

        case Syscall::CreateThread:
            returnValue(static_cast<u32>(createThread(arg(0))));
            return SyscallResult::Handled;

        case Syscall::StartThread:
            returnValue(static_cast<u32>(startThread(signed0(), arg(1))));
            return rescheduleIfWanted(interruptContext);

        case Syscall::DeleteThread:
            returnValue(static_cast<u32>(deleteThread(signed0())));
            return SyscallResult::Handled;

        case Syscall::ExitThread:
            return exitCurrentThread(false);

        case Syscall::ExitDeleteThread:
            return exitCurrentThread(true);

        case Syscall::TerminateThread:
        case Syscall::iTerminateThread:
            returnValue(static_cast<u32>(terminateThread(signed0(), interruptContext)));
            return rescheduleIfWanted(interruptContext);

        case Syscall::ChangeThreadPriority:
        case Syscall::iChangeThreadPriority:
            returnValue(static_cast<u32>(changeThreadPriority(
                signed0(), static_cast<s32>(arg(1)), interruptContext)));
            return rescheduleIfWanted(interruptContext);

        case Syscall::RotateThreadReadyQueue:
        case Syscall::iRotateThreadReadyQueue:
            returnValue(static_cast<u32>(rotateReadyQueue(signed0(), interruptContext)));
            return rescheduleIfWanted(interruptContext);

        case Syscall::ReleaseWaitThread:
        case Syscall::iReleaseWaitThread:
            returnValue(static_cast<u32>(releaseWaitThread(signed0(), interruptContext)));
            return rescheduleIfWanted(interruptContext);

        case Syscall::GetThreadId:
            returnValue(static_cast<u32>(mThreads.current()));
            return SyscallResult::Handled;

        case Syscall::ReferThreadStatus:
            returnValue(static_cast<u32>(referThreadStatus(signed0(), arg(1))));
            return SyscallResult::Handled;

        case Syscall::GetThreadCount:
            returnValue(static_cast<u32>(mThreads.count()));
            return SyscallResult::Handled;

        case Syscall::SleepThread:
            return sleepThread();

        case Syscall::WakeupThread:
        case Syscall::iWakeupThread:
            returnValue(static_cast<u32>(wakeupThread(signed0(), interruptContext)));
            return rescheduleIfWanted(interruptContext);

        case Syscall::CancelWakeupThread:
            returnValue(static_cast<u32>(cancelWakeupThread(signed0())));
            return SyscallResult::Handled;

        case Syscall::SuspendThread:
            returnValue(static_cast<u32>(suspendThread(signed0(), interruptContext)));
            return rescheduleIfWanted(interruptContext);

        case Syscall::ResumeThread:
        case Syscall::iResumeThread:
            returnValue(static_cast<u32>(resumeThread(signed0(), interruptContext)));
            return rescheduleIfWanted(interruptContext);

        case Syscall::DisableDispatchThread:
            mThreads.setDispatchDisabled(true);
            returnValue(static_cast<u32>(mThreads.current()));
            return SyscallResult::Handled;

        case Syscall::EnableDispatchThread:
            mThreads.setDispatchDisabled(false);
            returnValue(static_cast<u32>(mThreads.current()));
            return rescheduleIfWanted(interruptContext);

        case Syscall::CreateSema:
            returnValue(static_cast<u32>(createSemaphore(arg(0))));
            return SyscallResult::Handled;

        case Syscall::DeleteSema:
        case Syscall::iDeleteSema:
            returnValue(static_cast<u32>(deleteSemaphore(signed0(), interruptContext)));
            return rescheduleIfWanted(interruptContext);

        case Syscall::SignalSema:
        case Syscall::iSignalSema:
            returnValue(static_cast<u32>(signalSemaphore(signed0(), interruptContext)));
            return rescheduleIfWanted(interruptContext);

        case Syscall::WaitSema:
            return waitSemaphore(signed0());

        case Syscall::PollSema:
            returnValue(static_cast<u32>(pollSemaphore(signed0())));
            return SyscallResult::Handled;

        case Syscall::ReferSemaStatus:
            returnValue(static_cast<u32>(referSemaphoreStatus(signed0(), arg(1))));
            return SyscallResult::Handled;

        case Syscall::GetSemaCount:
            returnValue(static_cast<u32>(mSemaphores.count()));
            return SyscallResult::Handled;

        case Syscall::SetAlarm:
            returnValue(static_cast<u32>(
                setAlarm(static_cast<u16>(arg(0)), arg(1), arg(2))));
            return SyscallResult::Handled;

        case Syscall::ReleaseAlarm:
            returnValue(static_cast<u32>(releaseAlarm(signed0())));
            return SyscallResult::Handled;

        case Syscall::GetAlarmCount:
            returnValue(static_cast<u32>(mAlarms.count()));
            return SyscallResult::Handled;

        case Syscall::SetCpuTimerHandler:
            mCpuTimerHandler = arg(0);
            returnValue(0);
            return SyscallResult::Handled;

        case Syscall::SetCpuTimer:
            returnValue(0);
            return SyscallResult::Handled;

        case Syscall::SetSyscall:
            writeSyscallHandler(mMemory, signed0(), arg(1));
            returnValue(0);
            return SyscallResult::Handled;

        case Syscall::GetMemorySize:
            returnValue(kMainRamSize);
            return SyscallResult::Handled;

        case Syscall::MachineType:
            returnValue(mMachineType);
            return SyscallResult::Handled;

        // PSMode raises a bit in the word MachineType reports.
        case Syscall::PSMode:
            mMachineType |= kMachinePsMode;
            returnValue(0);
            return SyscallResult::Handled;

        case Syscall::SetOsdConfigParam:
        case Syscall::SetOsdConfigParam2:
            mOsdConfig = mMemory.read32(arg(0));
            returnValue(0);
            return SyscallResult::Handled;

        case Syscall::GetOsdConfigParam:
        case Syscall::GetOsdConfigParam2:
            mMemory.write32(arg(0), mOsdConfig);
            returnValue(0);
            return SyscallResult::Handled;

        case Syscall::FlushCache:
            returnValue(0);
            return SyscallResult::Handled;

        case Syscall::AddIntcHandler:
            returnValue(static_cast<u32>(
                addIntcHandler(arg(0), arg(1), static_cast<s32>(arg(2)), arg(3))));
            return SyscallResult::Handled;

        case Syscall::AddDmacHandler:
            returnValue(static_cast<u32>(
                addDmacHandler(arg(0), arg(1), static_cast<s32>(arg(2)), 0)));
            return SyscallResult::Handled;

        case Syscall::RemoveIntcHandler:
            returnValue(static_cast<u32>(mMmio.interrupts().removeIntcHandler(
                arg(0), static_cast<s32>(arg(1)))));
            return SyscallResult::Handled;

        case Syscall::RemoveDmacHandler:
            returnValue(static_cast<u32>(mMmio.interrupts().removeDmacHandler(
                arg(0), static_cast<s32>(arg(1)))));
            return SyscallResult::Handled;

        case Syscall::EnableIntc:
            returnValue(static_cast<u32>(enableIntc(arg(0), true)));
            return SyscallResult::Handled;

        case Syscall::DisableIntc:
            returnValue(static_cast<u32>(enableIntc(arg(0), false)));
            return SyscallResult::Handled;

        case Syscall::EnableDmac:
            returnValue(static_cast<u32>(enableDmac(arg(0), true)));
            return SyscallResult::Handled;

        case Syscall::DisableDmac:
            returnValue(static_cast<u32>(enableDmac(arg(0), false)));
            return SyscallResult::Handled;

        case Syscall::EnableIntcHandler:
        case Syscall::EnableDmacHandler:
            returnValue(static_cast<u32>(mMmio.interrupts().setHandlerState(
                static_cast<s32>(arg(0)), handler_state::kUser)));
            return SyscallResult::Handled;

        case Syscall::DisableIntcHandler:
        case Syscall::DisableDmacHandler:
            returnValue(static_cast<u32>(mMmio.interrupts().setHandlerState(
                static_cast<s32>(arg(0)), 1)));
            return SyscallResult::Handled;

        case Syscall::GetIntrHandlerCount:
            returnValue(static_cast<u32>(mMmio.interrupts().handlerCount()));
            return SyscallResult::Handled;

        // The run loop already restored the interrupted context.
        case Syscall::ResumeIntrDispatch:
            returnValue(0);
            return SyscallResult::Handled;

        case Syscall::SifSetDma:
            returnValue(sendSifDma(arg(0), arg(1)));
            return SyscallResult::Handled;

        case Syscall::SifSetReg:
            returnValue(writeSifRegister(arg(0), arg(1)));
            return SyscallResult::Handled;

        case Syscall::SifGetReg:
            returnValue(readSifRegister(arg(0)));
            return SyscallResult::Handled;

        case Syscall::SifSetDChain:
            returnValue(kSifDChainResult);
            return SyscallResult::Handled;

        // Every transfer has finished by the time the guest can ask.
        case Syscall::SifDmaStat:
            returnValue(0xFFFFFFFFu);
            return SyscallResult::Handled;

        // The ROM's shared stub: say so and carry on rather than end the run.
        case Syscall::Undefined:
            printUndefinedSyscall();
            return SyscallResult::Handled;

        case Syscall::GetCop0:
            returnValue(getCop0(arg(0)));
            return SyscallResult::Handled;

        case Syscall::CpuConfig:
            cpuConfig(arg(0));
            returnValue(0);
            return SyscallResult::Handled;

        case Syscall::KSeg0:
            setKernelSegmentMode(arg(0));
            returnValue(0);
            return SyscallResult::Handled;

        // No cache is modelled, so only the return value is observable.
        case Syscall::EnableCache:
        case Syscall::DisableCache:
        case Syscall::Rfu105:
            returnValue(0);
            return SyscallResult::Handled;

        case Syscall::SetVTlbRefillHandler:
            returnValue(static_cast<u32>(setVectorHandler(arg(0), arg(1), 1, 3)));
            return SyscallResult::Handled;

        case Syscall::SetVCommonHandler:
            returnValue(static_cast<u32>(setVectorHandler(arg(0), arg(1), 4, 13)));
            return SyscallResult::Handled;

        case Syscall::SetVInterruptHandler:
            returnValue(static_cast<u32>(setVectorHandler(arg(0), arg(1), 0, 7)));
            return SyscallResult::Handled;

        case Syscall::SetPgifHandler:
            mVectorHandlers[15] = arg(0);
            returnValue(0);
            return SyscallResult::Handled;

        case Syscall::SetVSyncFlag:
            mVSyncFlagAddress = arg(0);
            mVSyncTickAddress = arg(1);
            returnValue(0);
            return SyscallResult::Handled;

        case Syscall::Rfu009:
            returnValue(static_cast<u32>(writeTlbEntry(arg(1), arg(0), arg(2), arg(3))));
            return SyscallResult::Handled;

        case Syscall::InitTlb:
            mTlb = {};
            returnValue(0);
            return SyscallResult::Handled;

        case Syscall::GsGetIMR:
            mContext.cpu.write64(kReturnRegister, mGsImr);
            return SyscallResult::Handled;

        case Syscall::GsPutIMR: {
            const u64 imr = mContext.cpu.gpr[kFirstArgument].lo;
            mGsImr = imr;
            mMemory.write64(kGsImrRegister, imr);
            mContext.cpu.write64(kReturnRegister, imr);
            return SyscallResult::Handled;
        }

        case Syscall::SetGsHParam:
            for (u32 i = 0; i < 4; ++i) {
                mGsHParam[i] = arg(i);
            }
            returnValue(0);
            return SyscallResult::Handled;

        case Syscall::GetGsHParam:
            for (u32 i = 0; i < 4; ++i) {
                mMemory.write32(arg(i), mGsHParam[i]);
            }
            returnValue(mGsHParam[3]);
            return SyscallResult::Handled;

        case Syscall::SetGsVParam:
            mGsVParam = arg(0) & 1;
            returnValue(0);
            return SyscallResult::Handled;

        case Syscall::GetGsVParam:
            returnValue(mGsVParam | 0x80u);
            return SyscallResult::Handled;

        // The reported GS revision predates the offset, so every mode reads zero.
        case Syscall::GetGsDxDyOffset:
            for (u32 i = 1; i < 5; ++i) {
                if (arg(i) != 0) {
                    mMemory.write32(arg(i), 0);
                }
            }
            returnValue(0);
            return SyscallResult::Handled;

        case Syscall::SifStopDma:
            mMemory.write32(kSif0Chcr, 0);
            mMemory.write32(kSif0Qwc, 0);
            returnValue(0);
            return SyscallResult::Handled;

        case Syscall::AddSbusIntcHandler:
            returnValue(static_cast<u32>(
                arg(0) < 16 ? addIntcHandler(intc::kSbus, arg(1), -1, 0) : -1));
            return SyscallResult::Handled;

        case Syscall::RemoveSbusIntcHandler:
            returnValue(static_cast<u32>(mMmio.interrupts().removeIntcHandler(
                intc::kSbus, static_cast<s32>(arg(1)))));
            return SyscallResult::Handled;

        // Raising an SBUS interrupt needs an IOP to receive it.
        case Syscall::Interrupt2Iop:
            returnValue(static_cast<u32>(arg(0) < 16 ? 0 : -1));
            return SyscallResult::Handled;

        case Syscall::Print:
            printKernel(arg(0));
            returnValue(0);
            return SyscallResult::Handled;

        case Syscall::SetGsCrt:
            programGsCrt(static_cast<u16>(arg(0)), static_cast<u16>(arg(1)),
                         static_cast<u16>(arg(2)));
            returnValue(0);
            return SyscallResult::Handled;

        case Syscall::LoadExecPS2:
            log.stub("LoadExecPS2");
            returnValue(0xFFFFFFFFu);
            return SyscallResult::Handled;

        case Syscall::ExecOSD:
            log.stub("ExecOSD");
            returnValue(0xFFFFFFFFu);
            return SyscallResult::Handled;

        case Syscall::ResetEE:
            resetEE(arg(0));
            returnValue(0);
            return SyscallResult::Handled;

        case Syscall::ExecPS2:
            returnValue(execPS2(arg(0), arg(1), arg(2), arg(3)));
            return SyscallResult::Handled;

        // Drives the video encoder, which this runtime does not model.
        case Syscall::Rfu059:
            returnValue(0);
            return SyscallResult::Handled;

        default:
            return SyscallResult::Unknown;
    }
}

// KERNEL: SetupThread @ 0x80005190
u32 Kernel::setupThread(u32 gp, u32 stack, u32 stackSize, u32 root) {
    const u32 top = (stack == kUnusedStack) ? kMainRamSize : stack + stackSize;
    mStackBottom = top - stackSize;
    mRootThread = root;

    ThreadControlBlock& self = mThreads.at(mThreads.current());
    self.gp = gp;
    self.stack = mStackBottom;
    self.stackSize = static_cast<s32>(stackSize);
    self.rootFunction = root;
    self.context = top - kContextReserve;

    R5900& cpu = mContext.cpu;
    cpu.write64(kGlobalPointer, gp);
    cpu.write64(kStackPointer, top - kStackReserve);
    return top - kStackReserve;
}

// KERNEL: SetupHeap @ 0x80005298
u32 Kernel::setupHeap(u32 base, u32 size) {
    mHeapEnd = (size == kRestOfMemory) ? mStackBottom : base + size;
    mThreads.at(mThreads.current()).heapEnd = mHeapEnd;
    return mHeapEnd;
}

// KERNEL: CreateThread @ 0x80003c50
s32 Kernel::createThread(u32 param) {
    const s32 id = mThreads.allocate();
    if (id < 0) {
        return -1;
    }

    const ThreadControlBlock& creator = mThreads.at(mThreads.current());
    ThreadControlBlock& thread = mThreads.at(static_cast<usize>(id));

    const u32 stack = mMemory.read32(param + kThreadStack);
    const u32 stackSize = mMemory.read32(param + kThreadStackSize);
    const u32 entry = mMemory.read32(param + kThreadFunction);
    const u32 gp = mMemory.read32(param + kThreadGlobalPointer);
    const s16 priority = static_cast<s16>(mMemory.read32(param + kThreadPriority));

    thread.status = status::kDormant;
    thread.entry = entry;
    thread.resumePc = entry;
    thread.stack = stack;
    thread.stackSize = static_cast<s32>(stackSize);
    thread.gp = gp;
    thread.context = stack + stackSize - kContextReserve;
    thread.initialPriority = priority;
    thread.currentPriority = priority;
    thread.waitType = 0;
    thread.waitId = 0;
    thread.wakeupCount = 0;
    thread.argc = 0;
    thread.argv = 0;
    // CreateThread inherits both of these from its creator rather than reading
    // them out of the parameter block.
    thread.rootFunction = creator.rootFunction;
    thread.heapEnd = creator.heapEnd;

    EeContext fresh{};
    fresh.cpu.write64(kStackPointer, stack + stackSize - kStackReserve);
    fresh.cpu.write64(kGlobalPointer, gp);
    fresh.cpu.write64(kReturnAddress, creator.rootFunction);
    thread.saved.saveFrom(fresh);
    return id;
}

// KERNEL: StartThreadImpl @ 0x80003f68
s32 Kernel::startThread(s32 id, u32 argument) {
    if (!mThreads.isValid(id)) {
        return -1;
    }
    ThreadControlBlock& thread = mThreads.at(static_cast<usize>(id));
    if (thread.status != status::kDormant) {
        return -1;
    }
    thread.saved.gpr[kFirstArgument].lo = argument;
    thread.argv = argument;
    makeReadyAndPreempt(static_cast<usize>(id));
    return id;
}

// KERNEL: ResetThreadToDormant @ 0x80003bb8
void Kernel::resetToDormant(usize id) {
    ThreadControlBlock& thread = mThreads.at(id);
    const u32 top = thread.stack + static_cast<u32>(thread.stackSize);

    thread.status = status::kDormant;
    thread.waitType = 0;
    thread.resumePc = thread.entry;
    thread.context = top - kContextReserve;
    thread.currentPriority = thread.initialPriority;

    EeContext fresh{};
    fresh.cpu.write64(kStackPointer, top - kStackReserve);
    fresh.cpu.write64(kGlobalPointer, thread.gp);
    fresh.cpu.write64(kReturnAddress, thread.rootFunction);
    thread.saved.saveFrom(fresh);
}

// KERNEL: DeleteThreadImpl @ 0x80003ef8
s32 Kernel::deleteThread(s32 id) {
    if (id < 1 || !mThreads.isValid(id) || static_cast<usize>(id) == mThreads.current()) {
        return -1;
    }
    ThreadControlBlock& thread = mThreads.at(static_cast<usize>(id));
    if (thread.status != status::kDormant) {
        return -1;
    }
    mThreads.release(static_cast<usize>(id));
    thread.status = 0;
    return id;
}

// KERNEL: iTerminateThread @ 0x80003df8
s32 Kernel::terminateThread(s32 id, bool) {
    if (id < 1 || !mThreads.isValid(id)) {
        return -1;
    }
    ThreadControlBlock& thread = mThreads.at(static_cast<usize>(id));
    switch (thread.status) {
        case 0:
        case status::kDormant:
            return -1;

        // Terminating the running thread only asks for a reschedule and still
        // reports failure.
        case status::kRun:
            mThreads.requestReschedule();
            return -1;

        case status::kReady:
            mThreads.removeFromReadyQueue(static_cast<usize>(id));
            break;

        case status::kWait:
        case status::kWaitSuspend:
            if (thread.waitType == static_cast<s32>(WaitType::Semaphore)) {
                unlink(thread.link);
                --mSemaphores.at(static_cast<usize>(thread.waitId)).waitCount;
            }
            break;

        default:
            break;
    }
    resetToDormant(static_cast<usize>(id));
    return id;
}

// KERNEL: iChangeThreadPriority @ 0x80004280
s32 Kernel::changeThreadPriority(s32 id, s32 priority, bool) {
    if (!mThreads.isValid(id) || priority < 0 || priority >= kUserPriorities) {
        return -1;
    }
    const usize target = (id == 0) ? mThreads.current() : static_cast<usize>(id);
    ThreadControlBlock& thread = mThreads.at(target);
    if (thread.status == status::kDormant) {
        return -1;
    }
    const s32 previous = thread.currentPriority;
    if (thread.status == status::kRun || thread.status == status::kReady) {
        mThreads.removeFromReadyQueue(target);
        thread.currentPriority = static_cast<s16>(priority);
        mThreads.makeReady(target);
        if (target == mThreads.current()) {
            thread.status = status::kRun;
        }
        mThreads.requestReschedule();
    } else {
        thread.currentPriority = static_cast<s16>(priority);
    }
    return previous;
}

void Kernel::yieldCurrentThread() {
    const ThreadControlBlock& thread = mThreads.at(mThreads.current());
    mThreads.rotateReadyQueue(thread.currentPriority);
    mThreads.requestReschedule();
}

// KERNEL: iRotateThreadReadyQueue @ 0x80004380
s32 Kernel::rotateReadyQueue(s32 priority, bool) {
    if (priority < 0 || priority >= kUserPriorities) {
        return -1;
    }
    mThreads.rotateReadyQueue(priority);
    mThreads.requestReschedule();
    return priority;
}

// KERNEL: iReleaseWaitThread @ 0x800043c8
s32 Kernel::releaseWaitThread(s32 id, bool) {
    if (!mThreads.isValid(id) || id == 0) {
        return -1;
    }
    ThreadControlBlock& thread = mThreads.at(static_cast<usize>(id));
    if (thread.status != status::kWait && thread.status != status::kWaitSuspend) {
        return -1;
    }
    if (thread.waitType == static_cast<s32>(WaitType::Semaphore)) {
        unlink(thread.link);
        --mSemaphores.at(static_cast<usize>(thread.waitId)).waitCount;
    }
    // wait_type is deliberately left set: the dispatcher reads it as an
    // abnormal release and forces the blocked call to return -1.
    if (thread.status == status::kWaitSuspend) {
        thread.status = status::kSuspend;
    } else {
        makeReadyAndPreempt(static_cast<usize>(id));
    }
    return id;
}

// KERNEL: ReferThreadStatus @ 0x80004550
s32 Kernel::referThreadStatus(s32 id, u32 info) {
    if (!mThreads.isValid(id)) {
        return -1;
    }
    const usize target = (id == 0) ? mThreads.current() : static_cast<usize>(id);
    const ThreadControlBlock& thread = mThreads.at(target);
    if (info == 0) {
        return thread.status;
    }
    mMemory.write32(info + 0x00, static_cast<u32>(thread.status));
    mMemory.write32(info + 0x04, thread.resumePc);
    mMemory.write32(info + 0x08, thread.stack);
    mMemory.write32(info + 0x0C, static_cast<u32>(thread.stackSize));
    mMemory.write32(info + 0x10, thread.gp);
    mMemory.write32(info + 0x14, static_cast<u32>(thread.initialPriority));
    mMemory.write32(info + 0x18, static_cast<u32>(thread.currentPriority));
    mMemory.write32(info + 0x1C, thread.attr);
    mMemory.write32(info + 0x20, thread.option);
    mMemory.write32(info + 0x24, static_cast<u32>(thread.waitType));
    mMemory.write32(info + 0x28, static_cast<u32>(thread.waitId));
    mMemory.write32(info + 0x2C, static_cast<u32>(thread.wakeupCount));
    return thread.status;
}

// KERNEL: iWakeupThread @ 0x800046a8
s32 Kernel::wakeupThread(s32 id, bool) {
    if (!mThreads.isValid(id)) {
        return -1;
    }
    const usize target = (id == 0) ? mThreads.current() : static_cast<usize>(id);
    ThreadControlBlock& thread = mThreads.at(target);

    if (thread.status == status::kWait) {
        if (thread.waitType == static_cast<s32>(WaitType::Sleep)) {
            thread.waitType = 0;
            makeReadyAndPreempt(target);
            return static_cast<s32>(target);
        }
        ++thread.wakeupCount;
        return static_cast<s32>(target);
    }
    if (thread.status == status::kWaitSuspend) {
        if (thread.waitType == static_cast<s32>(WaitType::Sleep)) {
            thread.status = status::kSuspend;
            thread.waitType = 0;
            return static_cast<s32>(target);
        }
        ++thread.wakeupCount;
        return static_cast<s32>(target);
    }
    if (thread.status == status::kReady || thread.status == status::kSuspend) {
        ++thread.wakeupCount;
        return static_cast<s32>(target);
    }
    return -1;
}

// KERNEL: CancelWakeupThread @ 0x80004968
s32 Kernel::cancelWakeupThread(s32 id) {
    if (!mThreads.isValid(id)) {
        return -1;
    }
    const usize target = (id == 0) ? mThreads.current() : static_cast<usize>(id);
    ThreadControlBlock& thread = mThreads.at(target);
    const s32 previous = thread.wakeupCount;
    thread.wakeupCount = 0;
    return previous;
}

// KERNEL: SuspendThread @ 0x80004800
s32 Kernel::suspendThread(s32 id, bool) {
    if (id <= 0 || !mThreads.isValid(id)) {
        return -1;
    }
    ThreadControlBlock& thread = mThreads.at(static_cast<usize>(id));
    // Suspending the running thread does not switch away from it; it simply
    // stops being a candidate at the next reschedule.
    if (thread.status == status::kRun || thread.status == status::kReady) {
        mThreads.removeFromReadyQueue(static_cast<usize>(id));
        thread.status = status::kSuspend;
        return id;
    }
    if (thread.status == status::kWait) {
        thread.status = status::kWaitSuspend;
        return id;
    }
    return -1;
}

// KERNEL: iResumeThread @ 0x800048b8
s32 Kernel::resumeThread(s32 id, bool) {
    if (id < 1 || !mThreads.isValid(id) || static_cast<usize>(id) == mThreads.current()) {
        return -1;
    }
    ThreadControlBlock& thread = mThreads.at(static_cast<usize>(id));
    if (thread.status == status::kSuspend) {
        makeReadyAndPreempt(static_cast<usize>(id));
        return id;
    }
    if (thread.status == status::kWaitSuspend) {
        thread.status = status::kWait;
        return id;
    }
    return -1;
}

// KERNEL: SleepThread and SleepThreadImpl @ 0x800032c0
SyscallResult Kernel::sleepThread() {
    ThreadControlBlock& self = mThreads.at(mThreads.current());
    if (self.wakeupCount > 0) {
        --self.wakeupCount;
        returnValue(static_cast<u32>(mThreads.current()));
        return SyscallResult::Handled;
    }
    // A normal wakeup leaves this in place and the call returns the thread id;
    // an abnormal release overwrites it with -1.
    returnValue(static_cast<u32>(mThreads.current()));
    mThreads.removeFromReadyQueue(mThreads.current());
    blockCurrentAndDispatch(WaitType::Sleep);
    return mThreads.current() == static_cast<usize>(-1) ? SyscallResult::Deadlocked
                                                        : SyscallResult::Handled;
}

// KERNEL: ExitThreadImpl @ 0x80003ff0
SyscallResult Kernel::exitCurrentThread(bool andDelete) {
    const usize self = mThreads.current();
    mThreads.removeFromReadyQueue(self);
    resetToDormant(self);
    if (andDelete) {
        mThreads.release(self);
        mThreads.at(self).status = 0;
    }

    const s32 next = mThreads.chooseNext();
    if (next < 0) {
        // A thread waiting on an interrupt can still be made ready, so only a
        // table with nothing left in it ends the program.
        if (!mThreads.anyWaiting()) {
            return SyscallResult::Exited;
        }
        mIdle = true;
        return SyscallResult::Handled;
    }
    mIdle = false;
    // The reset context is the one a restart has to see, so the registers this
    // thread is exiting with are deliberately dropped.
    loadThread(static_cast<usize>(next));
    return SyscallResult::Handled;
}

// KERNEL: CreateSema @ 0x800049b8
s32 Kernel::createSemaphore(u32 param) {
    const s32 initial = static_cast<s32>(mMemory.read32(param + kSemaphoreInitialCount));
    if (initial < 0) {
        return -1;
    }
    const s32 id = mSemaphores.allocate();
    if (id < 0) {
        return -1;
    }
    SemaphoreControlBlock& semaphore = mSemaphores.at(static_cast<usize>(id));
    semaphore.count = initial;
    semaphore.maxCount = static_cast<s32>(mMemory.read32(param + kSemaphoreMaxCount));
    semaphore.attr = mMemory.read32(param + kSemaphoreAttr);
    semaphore.option = mMemory.read32(param + kSemaphoreOption);
    semaphore.waitCount = 0;
    initQueue(semaphore.waiters);
    return id;
}

// KERNEL: iDeleteSema @ 0x80004a40
s32 Kernel::deleteSemaphore(s32 id, bool) {
    if (!mSemaphores.isAlive(id)) {
        return -1;
    }
    SemaphoreControlBlock& semaphore = mSemaphores.at(static_cast<usize>(id));
    while (semaphore.waitCount > 0) {
        Link* node = removeTail(semaphore.waiters);
        if (node == nullptr) {
            break;
        }
        --semaphore.waitCount;
        const usize waiter = static_cast<usize>(
            reinterpret_cast<ThreadControlBlock*>(node) - &mThreads.at(0));
        ThreadControlBlock& thread = mThreads.at(waiter);
        // wait_type stays set, so every pending WaitSema returns -1.
        if (thread.status == status::kWaitSuspend) {
            thread.status = status::kSuspend;
        } else {
            makeReadyAndPreempt(waiter);
        }
    }
    mSemaphores.release(static_cast<usize>(id));
    return id;
}

// KERNEL: iSignalSema @ 0x80004bc0
s32 Kernel::signalSemaphore(s32 id, bool) {
    if (!mSemaphores.isAlive(id)) {
        return -1;
    }
    SemaphoreControlBlock& semaphore = mSemaphores.at(static_cast<usize>(id));
    if (semaphore.waitCount <= 0) {
        ++semaphore.count;
        return id;
    }

    Link* node = removeTail(semaphore.waiters);
    --semaphore.waitCount;
    const usize waiter =
        static_cast<usize>(reinterpret_cast<ThreadControlBlock*>(node) - &mThreads.at(0));
    ThreadControlBlock& thread = mThreads.at(waiter);
    // The token is handed straight over, so count is not incremented.
    if (thread.status == status::kWaitSuspend) {
        thread.status = status::kSuspend;
        thread.waitType = 0;
    } else if (thread.status == status::kWait) {
        thread.waitType = 0;
        makeReadyAndPreempt(waiter);
    }
    return id;
}

// KERNEL: WaitSema and WaitSemaImpl @ 0x80003440
SyscallResult Kernel::waitSemaphore(s32 id) {
    // The entry stub never stores the worker's result on the non-blocking path,
    // so the call returns the id it was passed even when that id is invalid.
    returnValue(static_cast<u32>(id));

    if (!mSemaphores.isAlive(id)) {
        return SyscallResult::Handled;
    }
    SemaphoreControlBlock& semaphore = mSemaphores.at(static_cast<usize>(id));
    if (semaphore.count > 0) {
        --semaphore.count;
        return SyscallResult::Handled;
    }

    const usize self = mThreads.current();
    ++semaphore.waitCount;
    mThreads.removeFromReadyQueue(self);
    insertHead(semaphore.waiters, mThreads.at(self).link);
    mThreads.at(self).waitId = id;
    blockCurrentAndDispatch(WaitType::Semaphore);
    return mThreads.current() == static_cast<usize>(-1) ? SyscallResult::Deadlocked
                                                        : SyscallResult::Handled;
}

// KERNEL: PollSema @ 0x80004dc0
s32 Kernel::pollSemaphore(s32 id) {
    if (!mSemaphores.isValid(id)) {
        return -1;
    }
    SemaphoreControlBlock& semaphore = mSemaphores.at(static_cast<usize>(id));
    if (semaphore.count <= 0) {
        return -1;
    }
    --semaphore.count;
    return id;
}

// KERNEL: ReferSemaStatus @ 0x80004df8
s32 Kernel::referSemaphoreStatus(s32 id, u32 info) {
    if (!mSemaphores.isAlive(id)) {
        return -1;
    }
    const SemaphoreControlBlock& semaphore = mSemaphores.at(static_cast<usize>(id));
    mMemory.write32(info + 0x00, static_cast<u32>(semaphore.count));
    mMemory.write32(info + 0x04, static_cast<u32>(semaphore.maxCount));
    mMemory.write32(info + 0x08, static_cast<u32>(semaphore.count));
    mMemory.write32(info + 0x0C, static_cast<u32>(semaphore.waitCount));
    mMemory.write32(info + 0x10, semaphore.attr);
    mMemory.write32(info + 0x14, semaphore.option);
    return id;
}

// KERNEL: AddIntcHandler @ 0x80001a30
s32 Kernel::addIntcHandler(u32 cause, u32 func, s32 next, u32 argument) {
    return mMmio.interrupts().addIntcHandler(cause, func, next, argument,
                                                 handler_state::kUser);
}

// KERNEL: AddDmacHandler @ 0x80001c78
s32 Kernel::addDmacHandler(u32 channel, u32 func, s32 next, u32 argument) {
    return mMmio.interrupts().addDmacHandler(channel, func, next, argument,
                                                 handler_state::kUser);
}

// The mask registers are write-one-to-toggle, so a bit is written only when it
// differs.
// KERNEL: ResetEE @ 0x8000d9b8
void Kernel::resetEE(u32 mask) {
    if ((mask & 0x01) != 0) {
        mOutput += "# Initialize DMAC ...\n";
        mMmio.interrupts().write(kDStat, 0xFFFFFFFFu);
        mMmio.interrupts().write(kDCtrl, 0);
        mMmio.interrupts().write(kDPcr, 0);
    }
    if ((mask & 0x02) != 0) {
        mOutput += "# Initialize VU1 ...\n";
    }
    if ((mask & 0x04) != 0) {
        mOutput += "# Initialize VIF1 ...\n";
    }
    if ((mask & 0x08) != 0) {
        mOutput += "# Initialize GIF ...\n";
    }
    if ((mask & 0x10) != 0) {
        mOutput += "# Initialize VU0 ...\n";
    }
    // VIF0 is gated on bit 2 here, the same bit as VIF1, not on the 0x20 the
    // published mask suggests.
    if ((mask & 0x04) != 0) {
        mOutput += "# Initialize VIF0 ...\n";
    }
    if ((mask & 0x40) != 0) {
        mOutput += "# Initialize IPU ...\n";
    }
    mMmio.interrupts().write(kIntcStat, ~0xCu);
}

// KERNEL: ExecPS2Internal @ 0x800057e0
u32 Kernel::execPS2(u32 entry, u32 argument, u32 argc, u32 argv) {
    const usize self = mThreads.current();
    cancelWakeupThread(static_cast<s32>(self));
    changeThreadPriority(static_cast<s32>(self), 0, true);

    for (usize id = 1; id < kThreadCount; ++id) {
        if (id == self || mThreads.at(id).status == 0) {
            continue;
        }
        if (mThreads.at(id).status != status::kDormant) {
            terminateThread(static_cast<s32>(id), true);
        }
        mThreads.release(id);
        mThreads.at(id).status = 0;
    }

    mSemaphores = SemaphoreTable{};
    mMmio.interrupts() = Interrupts{};
    mThreads.clearReschedule();
    mPendingCalls.clear();

    ThreadControlBlock& thread = mThreads.at(self);
    thread.resumePc = entry;
    thread.entry = entry;
    thread.context = argument;
    thread.argc = static_cast<s32>(argc);
    thread.argv = argv;
    thread.initialPriority = 0;
    thread.currentPriority = 0;
    thread.waitType = 0;
    thread.wakeupCount = 0;

    mContext.cpu.pc = entry;
    mContext.cpu.write64(kFirstArgument, argc);
    mContext.cpu.write64(kFirstArgument + 1, argv);
    return entry;
}

// KERNEL: UndefinedSyscall @ 0x80001564
void Kernel::printUndefinedSyscall() {
    const s32 raw = static_cast<s32>(static_cast<u32>(mContext.cpu.gpr[kSyscallRegister].lo));
    mOutput += std::format("# Syscall: undefined ({})\n", raw < 0 ? -raw : raw);
}

// KERNEL: GetCop0 @ 0x80002c40
u32 Kernel::getCop0(u32 reg) const {
    return reg < 32 ? mContext.cpu.cop0[reg] : 0;
}

// KERNEL: CpuConfig @ 0x80000a80
void Kernel::cpuConfig(u32 selector) {
    static constexpr u32 kBits[] = {kConfigBpe, kConfigIce, kConfigDie};
    if (selector >= 6) {
        return;
    }
    const u32 bit = kBits[selector % 3];
    u32& config = mContext.cpu.cop0[kCop0Config];
    config = selector < 3 ? (config | bit) : (config & ~bit);
}

// KERNEL: KSeg0 @ 0x80002c00
void Kernel::setKernelSegmentMode(u32 mode) {
    u32& config = mContext.cpu.cop0[kCop0Config];
    config = (config & ~kConfigKernelSegmentMask) | (mode & 3);
}

// KERNEL: SetVTLBRefillHandler @ 0x800009c0
s32 Kernel::setVectorHandler(u32 index, u32 handler, u32 first, u32 last) {
    if (index < first || index > last || index >= mVectorHandlers.size()) {
        return -1;
    }
    mVectorHandlers[index] = handler;
    return static_cast<s32>(index);
}

// KERNEL: RFU009 @ 0x80005bc8
s32 Kernel::writeTlbEntry(u32 pageMask, u32 entryHi, u32 entryLo0, u32 entryLo1) {
    // The ROM only honours the call when the entry's index byte says 4.
    if (((entryHi >> 24) & 0xFF) != 4) {
        return -1;
    }
    const u32 index = mContext.cpu.cop0[0] % mTlb.size();
    mTlb[index] = TlbEntry{pageMask, entryHi, entryLo0, entryLo1};
    return static_cast<s32>(index);
}

// KERNEL: _print @ 0x800074c8
void Kernel::printKernel(u32 format) {
    for (u32 i = 0; i < 1024; ++i) {
        const u8 c = mMemory.read8(format + i);
        if (c == 0) {
            break;
        }
        mOutput.push_back(static_cast<char>(c));
    }
}

// KERNEL: _EnableIntc and _DisableIntc @ 0x800008c0
s32 Kernel::enableIntc(u32 cause, bool enable) {
    Interrupts& interrupts = mMmio.interrupts();
    const u32 bit = 1u << cause;
    const bool already = (interrupts.read(kIntcMask).value_or(0) & bit) != 0;
    if (already == enable) {
        return 0;
    }
    interrupts.write(kIntcMask, bit);
    return 1;
}

// KERNEL: _EnableDmac and _DisableDmac @ 0x80000940
s32 Kernel::enableDmac(u32 channel, bool enable) {
    Interrupts& interrupts = mMmio.interrupts();
    const u32 bit = 0x10000u << channel;
    const bool already = (interrupts.read(kDStat).value_or(0) & bit) != 0;
    if (already == enable) {
        return 0;
    }
    interrupts.write(kDStat, bit);
    return 1;
}

// KERNEL: DispatchIntcHandlers @ 0x80001630
// KERNEL: InterruptHandler @ 0x80000554
// It clears the flag before running the chain, so only what the chain raises
// reaches the check on the way out.
void Kernel::queueHandlerChain(const std::vector<const IntrHandler*>& chain, u32 cause) {
    mThreads.clearReschedule();
    const usize before = mPendingCalls.size();
    ++mChains;
    for (const IntrHandler* handler : chain) {
        if (handler->state == handler_state::kUser && handler->func != 0) {
            mPendingCalls.push_back(GuestCall{handler->func, cause, handler->argument, 0, mChains});
        }
    }
    if (mPendingCalls.size() == before) {
        log.trace("interrupt {} has no handler to run, {} in the chain", cause, chain.size());
    }
}

// KERNEL: InterruptHandler @ 0x800004c0
void Kernel::deliverInterrupts() {
    if (!mPendingCalls.empty()) {
        if (++mCallsHeld == kHeldCallsReported) {
            log.warn("{} guest calls queued and not run, interrupts are held off",
                     mPendingCalls.size());
        }
        return;
    }
    mCallsHeld = 0;
    const u32 status = mContext.cpu.cop0[cop0::kStatus];
    const bool enabled = (status & cop0::kInterruptEnable) != 0 &&
                         (status & kStatusMasterEnable) != 0 &&
                         (status & cop0::kExceptionLevel) == 0 &&
                         (status & cop0::kErrorLevel) == 0;
    if (!enabled) {
        return;
    }

    Interrupts& interrupts = mMmio.interrupts();
    if ((status & kStatusIntc) != 0) {
        if (const std::optional<u32> cause = interrupts.pendingIntc()) {
            interrupts.acknowledgeIntc(*cause);
            log.trace("intc interrupt on cause {}", *cause);
            queueHandlerChain(interrupts.intcChain(*cause), *cause);
            return;
        }
    }
    if ((status & kStatusDmac) != 0) {
        if (const std::optional<u32> channel = interrupts.pendingDmac()) {
            interrupts.acknowledgeDmac(*channel);
            log.trace("dmac interrupt on channel {}", *channel);
            queueHandlerChain(interrupts.dmacChain(*channel), *channel);
        }
    }
}

// KERNEL: _SetAlarm @ 0x800022d8
s32 Kernel::setAlarm(u16 ticks, u32 callback, u32 argument) {
    const u32 gp = static_cast<u32>(mContext.cpu.gpr[kGlobalPointer].lo);
    return mAlarms.set(ticks, callback, argument, gp, mMmio.timers().hsyncCount());
}

// KERNEL: _ReleaseAlarm @ 0x80002570
s32 Kernel::releaseAlarm(s32 id) {
    return mAlarms.release(id, mMmio.timers().hsyncCount());
}

// KERNEL: SetGsCrt @ 0x8000bd58. These are the values its mode switch writes
// where the video encoder answers as a console's does and the configuration
// word it reads is zero. A renderer works the television standard out of
// SMODE1 and the field toggle out of SYNCV, so a display that is never
// programmed is one that cannot be shown.
void Kernel::programGsCrt(u16 interlace, u16 mode, u16 field) {
    constexpr u32 kSmode1 = 0x12000010, kSmode2 = 0x12000020, kSrfsh = 0x12000030;
    constexpr u32 kSynch1 = 0x12000040, kSynch2 = 0x12000050, kSyncv = 0x12000060;

    // Zero and one ask for whatever the console is; nothing here is a PAL one.
    const bool pal = mode == 3;
    if (mode > 3) {
        log.stub("SetGsCrt mode {}", mode);
        return;
    }

    mMemory.write64(kSmode1, pal ? 0x740816504ull : 0x740814504ull);
    mMemory.write64(kSynch1, pal ? 0x7F5C21FC83030ull : 0x7F5B61F06F040ull);
    mMemory.write64(kSynch2, pal ? 0x34897Cull : 0x33A798ull);
    mMemory.write64(kSrfsh, 8);
    if (interlace != 0) {
        mMemory.write64(kSyncv, pal ? 0xA9000502101401ull : 0xC7800601A01801ull);
        mMemory.write64(kSmode2, (static_cast<u64>(field) << 1) | 1);
    } else {
        mMemory.write64(kSyncv, pal ? 0xA9000502101404ull : 0xC7800601A01802ull);
        mMemory.write64(kSmode2, 0);
    }
}

void Kernel::advanceClock() {
    if (!mMmio.timers().advanceTo(mBudget.executed)) {
        return;
    }
    // A microprogram is long finished by the time the clock comes round, so the
    // status a guest reads next reports the unit idle again.
    mContext.vu0.statusSeen = 0;
    // A reply lands on a later tick, so a guest that starts a transfer and then
    // waits on its semaphore blocks first, as it would against real hardware.
    mMmio.gs().advanceScanline();
    mIop.advance(kIopInstructionsPerHsync, mIopBudgetPerHsync);
    if (mSif.advance()) {
        log.trace("sif0 reply delivered");
        mMmio.interrupts().raiseDmac(dmac::kSif0);
    }
    raiseFieldInterrupts();

    if (!mPendingCalls.empty()) {
        return;
    }
    std::vector<ExpiredAlarm> expired = mAlarms.takeExpired(mMmio.timers().hsyncCount());
    if (expired.empty()) {
        return;
    }

    // Timer 3 compare is INTC cause 12, so a guest handler on that line sees
    // the same event the alarm callbacks come from.
    log.trace("timer 3 compare, {} alarms due", expired.size());
    mMmio.interrupts().raiseIntc(intc::kTimer3);
    for (const ExpiredAlarm& alarm : expired) {
        mPendingCalls.push_back(GuestCall{alarm.callback, static_cast<u32>(alarm.id),
                                          alarm.target, alarm.argument});
    }
}

// KERNEL: the VSync flag handling in DispatchIntcHandlers @ 0x80001630
void Kernel::raiseFieldInterrupts() {
    const u32 line = mMmio.timers().hsyncCount() % kHsyncPerField;
    const bool blanking = line >= kVisibleLines;
    if (blanking == mInVblank) {
        return;
    }
    mInVblank = blanking;
    if (blanking) {
        mMmio.startVblank();
        mSpeed.countField();
    }
    log.trace("{} at scanline {}", blanking ? "vblank start" : "vblank end", line);
    mMmio.interrupts().raiseIntc(blanking ? intc::kVblankStart : intc::kVblankEnd);
    if (blanking) {
        mIop.startVblank();
    } else {
        mIop.endVblank();
    }

    // DispatchIntcHandlers satisfies a pending SetVSyncFlag at the start of the
    // blank, before it runs any handler, and clears both addresses.
    if (blanking && mVSyncFlagAddress != 0) {
        mMemory.write32(mVSyncFlagAddress, 1);
        mVSyncFlagAddress = 0;
        if (mVSyncTickAddress != 0) {
            mMemory.write64(mVSyncTickAddress, mMmio.gs().csr());
            mVSyncTickAddress = 0;
        }
    }
}

// Guest writes to the timer registers are not fed back, so only the syscall
// interface can reprogram a timer.
// KERNEL: MakeThreadReady @ 0x80005a58
void Kernel::makeReadyAndPreempt(usize id) {
    mThreads.makeReady(id);
    if (mThreads.at(id).currentPriority < mThreads.at(mThreads.current()).currentPriority) {
        mThreads.requestReschedule();
    }
}

// KERNEL: BlockCurrentAndDispatch @ 0x80003a78
void Kernel::blockCurrentAndDispatch(WaitType type) {
    ThreadControlBlock& self = mThreads.at(mThreads.current());
    self.status = status::kWait;
    self.waitType = static_cast<s32>(type);
    dispatchNext();
}

// KERNEL: the kprintf in DispatchNextThread @ 0x80003940. The ROM says it once
// and exits; this runtime says it when it gives up waiting for an interrupt.
void Kernel::reportNoActiveThreads() {
    mOutput += "# <Thread> No active threads\n";
}

// KERNEL: InterruptHandler @ 0x800005a0
// It dispatches on the way out when the chain raised the flag, so readying a
// sharper thread preempts rather than waiting for a syscall.
void Kernel::dispatchAfterInterrupt() {
    if (mIdle) {
        dispatchNext();
        return;
    }
    rescheduleIfWanted(false);
}

// KERNEL: DispatchNextThread @ 0x80003940
SyscallResult Kernel::dispatchNext() {
    const s32 next = mThreads.chooseNext();
    if (next < 0) {
        // The ROM calls KExit here, but an interrupt taken outside any thread
        // can still make one ready.
        mIdle = true;
        return SyscallResult::Handled;
    }
    mIdle = false;
    switchTo(static_cast<usize>(next));
    return SyscallResult::Handled;
}

// KERNEL: the k_RescheduleFlag check in the entry stubs @ 0x80003040
SyscallResult Kernel::rescheduleIfWanted(bool interruptContext) {
    if (!mThreads.rescheduleWanted()) {
        return SyscallResult::Handled;
    }
    // KERNEL: iTerminateThread @ 0x80003e5c
    // KERNEL: iChangeThreadPriority @ 0x80004338
    // Both only raise the flag, so clearing it here would lose the dispatch.
    if (interruptContext || mThreads.dispatchDisabled()) {
        return SyscallResult::Handled;
    }
    mThreads.clearReschedule();
    ThreadControlBlock& self = mThreads.at(mThreads.current());
    if (self.status == status::kRun) {
        self.status = status::kReady;
    }
    return dispatchNext();
}

// KERNEL: DispatchNextThread @ 0x80003940
void Kernel::loadThread(usize id) {
    mThreads.setCurrent(id);
    ThreadControlBlock& incoming = mThreads.at(id);
    incoming.status = status::kRun;
    // A wait still marked when the thread is picked was ended by force, so the
    // blocking call reports failure.
    if (incoming.waitType != 0) {
        incoming.waitType = 0;
        incoming.saved.gpr[kReturnRegister].lo = static_cast<u64>(static_cast<s64>(-1));
    }
    incoming.saved.loadInto(mContext);
    // CPCOND0 is driven by the DMAC, not owned by the thread whose registers
    // just came back, so it is restated rather than restored.
    mMmio.refreshCop0Condition();
    mContext.cpu.pc = incoming.resumePc;
}

// KERNEL: SaveThreadContext and DispatchNextThread @ 0x80003680
void Kernel::switchTo(usize id) {
    const usize previous = mThreads.current();
    if (id == previous) {
        mThreads.at(id).status = status::kRun;
        return;
    }
    ThreadControlBlock& outgoing = mThreads.at(previous);
    outgoing.saved.saveFrom(mContext);
    outgoing.resumePc = mContext.cpu.pc;
    loadThread(id);
}

bool Kernel::resumeFromSyscallHandler() {
    if (!mSyscallReturn) {
        return false;
    }
    const SyscallReturn saved = *mSyscallReturn;
    mSyscallReturn.reset();
    mContext.cpu.write64(kReturnAddress, saved.returnAddress);
    mContext.cpu.pc = saved.resumePc;
    return true;
}

// A negative answer ends the handler's own chain, not one an interrupt started
// while it ran.
void Kernel::completePendingCall(u32 result) {
    if (static_cast<s32>(result) >= 0 || mRunningChain == 0) {
        return;
    }
    while (!mPendingCalls.empty() && mPendingCalls.front().chain == mRunningChain) {
        mPendingCalls.erase(mPendingCalls.begin());
    }
}

std::optional<GuestCall> Kernel::takePendingCall() {
    if (mPendingCalls.empty()) {
        return std::nullopt;
    }
    const GuestCall call = mPendingCalls.front();
    mPendingCalls.erase(mPendingCalls.begin());
    mRunningChain = call.chain;
    return call;
}

// KERNEL: Deci2CallHandler @ 0x8000ecc8. Only the kernel TTY is answered; the
// rest of the manager talks to a host debugger over a link that is not here.
s32 Kernel::deci2Call(u32 function, u32 arguments) {
    if (function != kKernelTty) {
        log.stub("Deci2Call {}", function);
        return -1;
    }
    const u32 text = mMemory.read32(arguments);
    for (u32 i = 0; i < kTtyLimit; ++i) {
        const char letter = static_cast<char>(mMemory.read8(text + i));
        if (letter == '\0') {
            break;
        }
        mOutput.push_back(letter);
    }
    return 0;
}

// KERNEL: SifGetReg @ 0x80006cb8
u32 Kernel::readSifRegister(u32 id) {
    const auto sbusRegister = [this](u32 address) {
        return mMmio.sbus().read(address).value_or(0);
    };
    switch (id) {
        case kMainToSubCommand: return sbusRegister(sbus::kMainToSubCommand);
        case kSubToMainCommand: return sbusRegister(sbus::kSubToMainCommand);
        case kMainToSubFlag: return sbusRegister(sbus::kMainToSubFlag);
        case kSubToMainFlag: return sbusRegister(sbus::kSubToMainFlag);
        default: break;
    }
    if ((id & kSoftwareRegister) == 0 || (id & ~kSoftwareRegister) >= kSoftwareRegisters) {
        return 0;
    }
    return mSifRegisters[id & (kSoftwareRegisters - 1)];
}

// KERNEL: SifSetReg @ 0x80006c00. The hardware writes answer with the register
// read back, which is not the same as the value written for either flag word.
u32 Kernel::writeSifRegister(u32 id, u32 value) {
    switch (id) {
        case kMainToSubCommand:
            mMmio.sbus().write(sbus::kMainToSubCommand, value);
            return readSifRegister(kMainToSubCommand);
        case kMainToSubFlag:
            mMmio.sbus().write(sbus::kMainToSubFlag, value);
            return readSifRegister(kMainToSubFlag);
        case kSubToMainFlag:
            mMmio.sbus().write(sbus::kSubToMainFlag, value);
            return readSifRegister(kSubToMainFlag);
        default: break;
    }
    if ((id & kSoftwareRegister) == 0 || (id & ~kSoftwareRegister) >= kSoftwareRegisters) {
        return 0;
    }
    mSifRegisters[id & (kSoftwareRegisters - 1)] = value;
    return readArgument(2);
}

// KERNEL: SifSetDma @ 0x80006798
u32 Kernel::sendSifDma(u32 transfers, u32 count) {
    mSif.receive(transfers, count);
    // The channel the transfer went out on reports itself finished, which is
    // what a guest waiting on the controller rather than the interface sees.
    mMmio.interrupts().raiseDmac(dmac::kSif1);
    return ++mDmaTransferId;
}

void Kernel::reportThreads() const {
    mSif.report();
    for (usize i = 0; i < kThreadCount; ++i) {
        const ThreadControlBlock& thread = mThreads.at(i);
        if (thread.status == 0 || thread.status == status::kDormant) {
            continue;
        }
        log.info("thread {} status {} priority {} wait {} id {} pc {} ra {} entry {}", i,
                 thread.status, thread.currentPriority, thread.waitType, thread.waitId,
                 mSymbols.describe(i == mThreads.current() ? mContext.cpu.pc : thread.resumePc),
                 mSymbols.describe(static_cast<u32>(mThreads.at(i).saved.gpr[kReturnAddress].lo)),
                 mSymbols.describe(thread.entry));
    }
    mIop.reportThreads();
}

}  // namespace uee
