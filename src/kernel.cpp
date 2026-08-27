#include "kernel.h"

#include "memory.h"
#include "sif.h"

#include <format>
#include <stdexcept>
#include <utility>

namespace uee {
namespace {

constexpr u32 kStackReserve = 0x40;

constexpr u32 kStackPointer = 29;
constexpr u32 kGlobalPointer = 28;
constexpr u32 kReturnAddress = 31;
constexpr u32 kFirstArgument = 4;
constexpr u32 kReturnRegister = 2;
constexpr u32 kSyscallRegister = 3;

constexpr u32 kThreadFunction = 4;
constexpr u32 kThreadStack = 8;
constexpr u32 kThreadStackSize = 12;
constexpr u32 kThreadGlobalPointer = 16;
constexpr u32 kThreadPriority = 20;

constexpr u32 kSemaphoreMaxCount = 4;
constexpr u32 kSemaphoreInitialCount = 8;

constexpr u32 kUnusedStack = 0xFFFFFFFFu;
constexpr u32 kRestOfMemory = 0xFFFFFFFFu;

constexpr u32 kWriteFileDescriptor = 0;
constexpr u32 kWriteBuffer = 4;
constexpr u32 kWriteSize = 8;

constexpr s32 kStandardOutput = 1;
constexpr s32 kStandardError = 2;
constexpr s32 kFileIoWrite = 3;

// The IOP address the fileio server would hand back at bind time. Nothing
// dereferences it here, but the guest refuses to call an unbound server.
constexpr u32 kFileIoServerBuffer = 0x00010000;

enum class SifRegister : u32 {
    SubAddress = 2,
    MainToSubFlag = 3,
    SubToMainFlag = 4,
    SystemSubAddress = 0x80000000,
    SystemMainAddress = 0x80000001,
    SystemRpcInitialised = 0x80000002,
};

constexpr u32 kSifInitialised = 0x10000;
constexpr u32 kCommandInitialised = 0x20000;
constexpr u32 kBootFinished = 0x40000;

enum class Syscall : s32 {
    Exit = 4,
    AddIntcHandler = 16,
    RemoveIntcHandler = 17,
    AddDmacHandler = 18,
    RemoveDmacHandler = 19,
    EnableIntc = 20,
    DisableIntc = 21,
    EnableDmac = 22,
    DisableDmac = 23,
    CreateThread = 32,
    StartThread = 34,
    ExitThread = 35,
    ExitDeleteThread = 36,
    SetupThread = 60,
    SetupHeap = 61,
    EndOfHeap = 62,
    CreateSema = 64,
    DeleteSema = 65,
    SignalSema = 66,
    SignalSemaFromInterrupt = 67,
    WaitSema = 68,
    SetOsdConfigParam = 74,
    GetOsdConfigParam = 75,
    FlushCache = 100,
    SetSyscall = 116,
    SifDmaStat = 118,
    SifSetDma = 119,
    SifSetDChain = 120,
    SifSetReg = 121,
    SifGetReg = 122,
};

}  // namespace

Kernel::Kernel(EmotionEngine& machine, Memory& memory)
    : mMachine(machine), mMemory(memory) {
    Thread main;
    main.state = ThreadState::Ready;
    mThreads.push_back(main);

    // SIF init spins until the IOP reports itself up, and there is no IOP here
    // to report it. Both spins are skipped when the guest finds the SIF and RPC
    // layers already initialised.
    const u32 ready = kSifInitialised | kCommandInitialised | kBootFinished;
    writeSifRegister(static_cast<u32>(SifRegister::MainToSubFlag), ready);
    writeSifRegister(static_cast<u32>(SifRegister::SubToMainFlag), ready);
    writeSifRegister(static_cast<u32>(SifRegister::SubAddress), kFileIoServerBuffer);
    writeSifRegister(static_cast<u32>(SifRegister::SystemSubAddress), kFileIoServerBuffer);
    writeSifRegister(static_cast<u32>(SifRegister::SystemRpcInitialised), 1);
}

u32 Kernel::readArgument(u32 index) const {
    return static_cast<u32>(mMachine.cpu.gpr[kFirstArgument + index].lo);
}

void Kernel::returnValue(u32 value) {
    mMachine.cpu.write32(kReturnRegister, value);
}

SyscallResult Kernel::handleSyscall() {
    const s32 raw = static_cast<s32>(static_cast<u32>(mMachine.cpu.gpr[kSyscallRegister].lo));
    // A negative number reaches the same table entry as its absolute value; the
    // sign only tells the real kernel it was called from interrupt context.
    switch (static_cast<Syscall>(raw < 0 ? -raw : raw)) {
        case Syscall::Exit:
            return SyscallResult::Exited;

        case Syscall::SetupThread:
            returnValue(setupThread(readArgument(0), readArgument(1), readArgument(2),
                                    readArgument(4)));
            return SyscallResult::Handled;

        case Syscall::SetupHeap:
            returnValue(setupHeap(readArgument(0), readArgument(1)));
            return SyscallResult::Handled;

        case Syscall::EndOfHeap:
            returnValue(mHeapEnd);
            return SyscallResult::Handled;

        case Syscall::CreateThread:
            returnValue(static_cast<u32>(createThread(readArgument(0))));
            return SyscallResult::Handled;

        case Syscall::StartThread:
            returnValue(static_cast<u32>(
                startThread(static_cast<s32>(readArgument(0)), readArgument(1))));
            return SyscallResult::Handled;

        case Syscall::ExitThread:
        case Syscall::ExitDeleteThread:
            return exitCurrentThread();

        case Syscall::CreateSema:
            returnValue(static_cast<u32>(createSemaphore(readArgument(0))));
            return SyscallResult::Handled;

        case Syscall::DeleteSema:
            findSemaphore(static_cast<s32>(readArgument(0))).isAlive = false;
            returnValue(readArgument(0));
            return SyscallResult::Handled;

        case Syscall::SignalSema:
        case Syscall::SignalSemaFromInterrupt:
            signalSemaphore(static_cast<s32>(readArgument(0)));
            returnValue(readArgument(0));
            return SyscallResult::Handled;

        case Syscall::WaitSema:
            return waitSemaphore(static_cast<s32>(readArgument(0)));

        case Syscall::FlushCache:
        case Syscall::SetOsdConfigParam:
        case Syscall::GetOsdConfigParam:
            returnValue(0);
            return SyscallResult::Handled;

        // The guest replaces kernel entries this kernel implements itself, so
        // the handler it supplies is recorded nowhere and never dispatched to.
        case Syscall::SetSyscall:
            returnValue(0);
            return SyscallResult::Handled;

        // Nothing in this runtime raises an interrupt, so a registered handler
        // can never run and registering one only has to look like it worked.
        case Syscall::AddIntcHandler:
        case Syscall::AddDmacHandler:
            returnValue(++mHandlerCount);
            return SyscallResult::Handled;

        case Syscall::RemoveIntcHandler:
        case Syscall::RemoveDmacHandler:
        case Syscall::EnableIntc:
        case Syscall::DisableIntc:
        case Syscall::EnableDmac:
        case Syscall::DisableDmac:
            returnValue(1);
            return SyscallResult::Handled;

        case Syscall::SifSetDma:
            returnValue(sendSifDma(readArgument(0), readArgument(1)));
            return SyscallResult::Handled;

        case Syscall::SifSetReg:
            writeSifRegister(readArgument(0), readArgument(1));
            returnValue(0);
            return SyscallResult::Handled;

        case Syscall::SifGetReg:
            returnValue(readSifRegister(readArgument(0)));
            return SyscallResult::Handled;

        case Syscall::SifSetDChain:
            returnValue(0);
            return SyscallResult::Handled;

        // Every transfer this kernel accepts has already finished by the time
        // the guest can ask about it.
        case Syscall::SifDmaStat:
            returnValue(0xFFFFFFFFu);
            return SyscallResult::Handled;
    }
    return SyscallResult::Unknown;
}

u32 Kernel::setupThread(u32 gp, u32 stack, u32 stackSize, u32 root) {
    const u32 top = (stack == kUnusedStack) ? kMainRamSize : stack + stackSize;
    mStackBottom = top - stackSize;
    mRootThread = root;

    R5900& cpu = mMachine.cpu;
    cpu.write64(kGlobalPointer, gp);
    cpu.write64(kStackPointer, top - kStackReserve);
    return top - kStackReserve;
}

u32 Kernel::setupHeap(u32 base, u32 size) {
    mHeapEnd = (size == kRestOfMemory) ? mStackBottom : base + size;
    return mHeapEnd;
}

s32 Kernel::createThread(u32 param) {
    Thread thread;
    thread.state = ThreadState::Dormant;
    thread.priority = static_cast<s32>(mMemory.read32(param + kThreadPriority));

    const u32 stack = mMemory.read32(param + kThreadStack);
    const u32 stackSize = mMemory.read32(param + kThreadStackSize);

    R5900& cpu = thread.context.cpu;
    cpu.pc = mMemory.read32(param + kThreadFunction);
    cpu.write64(kStackPointer, stack + stackSize - kStackReserve);
    cpu.write64(kGlobalPointer, mMemory.read32(param + kThreadGlobalPointer));
    cpu.write64(kReturnAddress, mRootThread);

    mThreads.push_back(thread);
    return static_cast<s32>(mThreads.size());
}

s32 Kernel::startThread(s32 id, u32 argument) {
    Thread& thread = mThreads.at(static_cast<usize>(id) - 1);
    thread.state = ThreadState::Ready;
    thread.context.cpu.write64(kFirstArgument, argument);
    reschedule();
    return id;
}

SyscallResult Kernel::exitCurrentThread() {
    mThreads[mCurrent].state = ThreadState::Dead;
    const usize next = chooseReadyThread();
    if (mThreads[next].state != ThreadState::Ready) {
        return SyscallResult::Exited;
    }
    switchTo(next);
    return SyscallResult::Handled;
}

s32 Kernel::createSemaphore(u32 param) {
    Semaphore semaphore;
    semaphore.count = static_cast<s32>(mMemory.read32(param + kSemaphoreInitialCount));
    semaphore.maxCount = static_cast<s32>(mMemory.read32(param + kSemaphoreMaxCount));
    semaphore.isAlive = true;
    mSemaphores.push_back(semaphore);
    return static_cast<s32>(mSemaphores.size());
}

Kernel::Semaphore& Kernel::findSemaphore(s32 id) {
    const usize index = static_cast<usize>(id) - 1;
    if (id < 1 || index >= mSemaphores.size() || !mSemaphores[index].isAlive) {
        throw std::runtime_error(std::format("semaphore {} does not exist", id));
    }
    return mSemaphores[index];
}

SyscallResult Kernel::waitSemaphore(s32 id) {
    Semaphore& semaphore = findSemaphore(id);
    returnValue(static_cast<u32>(id));
    if (semaphore.count > 0) {
        --semaphore.count;
        return SyscallResult::Handled;
    }

    mThreads[mCurrent].state = ThreadState::Waiting;
    mThreads[mCurrent].waitingOn = id;
    const usize next = chooseReadyThread();
    if (mThreads[next].state != ThreadState::Ready) {
        return SyscallResult::Deadlocked;
    }
    switchTo(next);
    return SyscallResult::Handled;
}

void Kernel::signalSemaphore(s32 id) {
    Semaphore& semaphore = findSemaphore(id);

    for (Thread& thread : mThreads) {
        if (thread.state == ThreadState::Waiting && thread.waitingOn == id) {
            thread.state = ThreadState::Ready;
            reschedule();
            return;
        }
    }
    if (semaphore.count < semaphore.maxCount) {
        ++semaphore.count;
    }
}

usize Kernel::chooseReadyThread() const {
    usize best = mCurrent;
    for (usize i = 0; i < mThreads.size(); ++i) {
        if (mThreads[i].state != ThreadState::Ready) {
            continue;
        }
        if (mThreads[best].state != ThreadState::Ready ||
            mThreads[i].priority < mThreads[best].priority) {
            best = i;
        }
    }
    return best;
}

void Kernel::switchTo(usize index) {
    if (index == mCurrent) {
        return;
    }
    mThreads[mCurrent].context = mMachine;
    mCurrent = index;
    mMachine = mThreads[mCurrent].context;
}

void Kernel::reschedule() {
    switchTo(chooseReadyThread());
}

std::optional<GuestCall> Kernel::takePendingCall() {
    return std::exchange(mPendingCall, std::nullopt);
}

u32 Kernel::readSifRegister(u32 id) const {
    const auto found = mSifRegisters.find(id);
    return found == mSifRegisters.end() ? 0 : found->second;
}

void Kernel::writeSifRegister(u32 id, u32 value) {
    mSifRegisters[id] = value;
}

u32 Kernel::sendSifDma(u32 transfers, u32 count) {
    const SifRequest request = decodeSifDma(mMemory, transfers, count);

    switch (request.command) {
        case SifCommand::Bind:
            writeBindReply(mMemory, request.clientData, kFileIoServerBuffer);
            break;

        case SifCommand::Call: {
            if (request.rpcNumber != kFileIoWrite) {
                throw std::runtime_error(std::format(
                    "fileio command {} is not implemented; only write is", request.rpcNumber));
            }
            const s32 result = writeFile(request.sendBuffer);
            if (request.receiveSize >= 4) {
                mMemory.write32(request.receiveBuffer, static_cast<u32>(result));
            }
            if (request.endFunction != 0) {
                mPendingCall = GuestCall{request.endFunction, request.endParameter};
            }
            break;
        }

        case SifCommand::Other:
            return ++mDmaTransferId;
    }

    releaseRequest(mMemory, request);
    if (request.semaphore >= 0) {
        signalSemaphore(request.semaphore);
    }
    return ++mDmaTransferId;
}

s32 Kernel::writeFile(u32 argument) {
    const s32 descriptor = static_cast<s32>(mMemory.read32(argument + kWriteFileDescriptor));
    const u32 buffer = mMemory.read32(argument + kWriteBuffer);
    const u32 size = mMemory.read32(argument + kWriteSize);

    if (descriptor != kStandardOutput && descriptor != kStandardError) {
        throw std::runtime_error(
            std::format("write to file descriptor {}, which nothing backs", descriptor));
    }

    mOutput.reserve(mOutput.size() + size);
    for (u32 i = 0; i < size; ++i) {
        mOutput.push_back(static_cast<char>(mMemory.read8(buffer + i)));
    }
    return static_cast<s32>(size);
}

}  // namespace uee
