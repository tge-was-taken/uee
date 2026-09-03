#pragma once
#include "memory.h"
#include "types.h"

#include <string_view>

namespace uee {

constexpr u32 kSyscallTableBase = 0x80014f40;
constexpr s32 kSyscallSlots = 131;
constexpr u32 kKernelTextBase = 0x80000000;
constexpr u32 kKernelTextEnd = 0x80016e28;

// Keyed by the ROM address, so a guest that saves and restores a slot gets the
// built-in implementation back.
enum class Syscall : u32 {
    Undefined = 0x80001564,
    ResetEE = 0x8000d9b8,
    SetGsCrt = 0x8000bd58,
    Exit = 0x80000d80,
    ResumeIntrDispatch = 0x80002880,
    LoadExecPS2 = 0x80005598,
    ExecPS2 = 0x80002f80,
    Rfu009 = 0x80005bc8,
    AddSbusIntcHandler = 0x80001e78,
    RemoveSbusIntcHandler = 0x80001ed0,
    Interrupt2Iop = 0x80001f70,
    SetVTlbRefillHandler = 0x800009c0,
    SetVCommonHandler = 0x80000a00,
    SetVInterruptHandler = 0x80000a40,
    AddIntcHandler = 0x80001a30,
    RemoveIntcHandler = 0x80001a70,
    AddDmacHandler = 0x80001c78,
    RemoveDmacHandler = 0x80001cb8,
    EnableIntc = 0x800008c0,
    DisableIntc = 0x80000900,
    EnableDmac = 0x80000940,
    DisableDmac = 0x80000980,
    SetAlarm = 0x800022d8,
    ReleaseAlarm = 0x80002570,
    CreateThread = 0x80003c50,
    DeleteThread = 0x80002fc0,
    StartThread = 0x80003040,
    ExitThread = 0x800030c0,
    ExitDeleteThread = 0x80003100,
    TerminateThread = 0x80003140,
    iTerminateThread = 0x80003df8,
    DisableDispatchThread = 0x80004220,
    EnableDispatchThread = 0x80004250,
    ChangeThreadPriority = 0x800035c0,
    iChangeThreadPriority = 0x80004280,
    RotateThreadReadyQueue = 0x800031c0,
    iRotateThreadReadyQueue = 0x80004380,
    ReleaseWaitThread = 0x80003240,
    iReleaseWaitThread = 0x800043c8,
    GetThreadId = 0x80004540,
    ReferThreadStatus = 0x80004550,
    SleepThread = 0x800032c0,
    WakeupThread = 0x80003340,
    iWakeupThread = 0x800046a8,
    CancelWakeupThread = 0x80004968,
    SuspendThread = 0x80004800,
    iResumeThread = 0x800048b8,
    ResumeThread = 0x800033c0,
    Rfu059 = 0x8000cfb0,
    SetupThread = 0x80005190,
    SetupHeap = 0x80005298,
    EndOfHeap = 0x800052d0,
    CreateSema = 0x800049b8,
    DeleteSema = 0x80003540,
    SignalSema = 0x800034c0,
    iSignalSema = 0x80004bc0,
    WaitSema = 0x80003440,
    PollSema = 0x80004dc0,
    ReferSemaStatus = 0x80004df8,
    iDeleteSema = 0x80004a40,
    SetOsdConfigParam = 0x8000d470,
    GetOsdConfigParam = 0x8000d3c0,
    GetGsHParam = 0x8000cd18,
    GetGsVParam = 0x8000cf58,
    SetGsHParam = 0x8000cfd0,
    SetGsVParam = 0x8000d260,
    GetThreadCount = 0x800049a8,
    GetSemaCount = 0x80004e50,
    GetAlarmCount = 0x800027e8,
    GetIntrHandlerCount = 0x80002040,
    EnableIntcHandler = 0x80001fa0,
    DisableIntcHandler = 0x80001fc8,
    EnableDmacHandler = 0x80001ff0,
    DisableDmacHandler = 0x80002018,
    KSeg0 = 0xa0002c00,
    EnableCache = 0xa00028c0,
    DisableCache = 0xa0002980,
    GetCop0 = 0x80002c40,
    FlushCache = 0x80002a40,
    Rfu105 = 0x80002b40,
    CpuConfig = 0x80000a80,
    SifStopDma = 0x80006378,
    SetCpuTimerHandler = 0x800007c0,
    SetCpuTimer = 0x80000800,
    SetOsdConfigParam2 = 0x8000d350,
    GetOsdConfigParam2 = 0x8000d2b8,
    GsGetIMR = 0x80000d00,
    GsPutIMR = 0x80000d40,
    SetPgifHandler = 0x80000740,
    SetVSyncFlag = 0x80001588,
    SetSyscall = 0x800006c0,
    Print = 0x800074c8,
    SifDmaStat = 0x80006950,
    SifSetDma = 0x80006798,
    SifSetDChain = 0x80006340,
    SifSetReg = 0x80006c00,
    SifGetReg = 0x80006cb8,
    ExecOSD = 0x80005988,
    PSMode = 0x80000b80,
    MachineType = 0x80000bc0,
    GetMemorySize = 0x80000c40,
    GetGsDxDyOffset = 0x8000cca0,
    InitTlb = 0x80005c30,
};
// Writes the ROM table into guest memory so SetSyscall can rewrite it and a
// guest can read it back.
void installSyscallTable(Memory& memory);
u32 readSyscallHandler(Memory& memory, s32 number);
// What the BIOS puts in the slot, which is what runs unless the game
// replaces it with SetSyscall.
u32 getDefaultSyscallHandler(s32 number);
void writeSyscallHandler(Memory& memory, s32 number, u32 handler);
constexpr bool isKernelHandler(u32 handler) {
    return (handler >= kKernelTextBase && handler < kKernelTextEnd) ||
           (handler >= 0xa0000000 && handler < 0xa0016e28);
}
std::string_view nameOfSyscall(s32 number);
bool isInterruptSyscall(s32 number);

// One missing from this list is assumed to, so a new one is safe by default.
constexpr bool canYield(Syscall syscall) {
    switch (syscall) {
        case Syscall::GetThreadId:
        case Syscall::ReferThreadStatus:
        case Syscall::GetThreadCount:
        case Syscall::GetSemaCount:
        case Syscall::GetAlarmCount:
        case Syscall::GetIntrHandlerCount:
        case Syscall::ReferSemaStatus:
        case Syscall::PollSema:
        case Syscall::GetOsdConfigParam:
        case Syscall::GetOsdConfigParam2:
        case Syscall::SetOsdConfigParam:
        case Syscall::SetOsdConfigParam2:
        case Syscall::GetGsHParam:
        case Syscall::GetGsVParam:
        case Syscall::SetGsHParam:
        case Syscall::SetGsVParam:
        case Syscall::GetGsDxDyOffset:
        case Syscall::GsGetIMR:
        case Syscall::GsPutIMR:
        case Syscall::SetGsCrt:
        case Syscall::SetVSyncFlag:
        case Syscall::GetCop0:
        case Syscall::FlushCache:
        case Syscall::KSeg0:
        case Syscall::EnableCache:
        case Syscall::DisableCache:
        case Syscall::CpuConfig:
        case Syscall::GetMemorySize:
        case Syscall::MachineType:
        case Syscall::PSMode:
        case Syscall::SetupHeap:
        case Syscall::EndOfHeap:
        case Syscall::InitTlb:
        case Syscall::SetVTlbRefillHandler:
        case Syscall::SetVCommonHandler:
        case Syscall::SetVInterruptHandler:
        case Syscall::AddIntcHandler:
        case Syscall::RemoveIntcHandler:
        case Syscall::AddDmacHandler:
        case Syscall::RemoveDmacHandler:
        case Syscall::AddSbusIntcHandler:
        case Syscall::RemoveSbusIntcHandler:
        case Syscall::EnableIntc:
        case Syscall::DisableIntc:
        case Syscall::EnableDmac:
        case Syscall::DisableDmac:
        case Syscall::EnableIntcHandler:
        case Syscall::DisableIntcHandler:
        case Syscall::EnableDmacHandler:
        case Syscall::DisableDmacHandler:
        case Syscall::SetCpuTimerHandler:
        case Syscall::SetCpuTimer:
        case Syscall::SetPgifHandler:
        case Syscall::SetAlarm:
        case Syscall::ReleaseAlarm:
        case Syscall::SifStopDma:
        case Syscall::SifDmaStat:
        case Syscall::SifSetDma:
        case Syscall::SifSetDChain:
        case Syscall::SifSetReg:
        case Syscall::SifGetReg:
        case Syscall::CreateThread:
        case Syscall::CreateSema:
        case Syscall::Print:
            return false;
        default:
            return true;
    }
}

}  // namespace uee
