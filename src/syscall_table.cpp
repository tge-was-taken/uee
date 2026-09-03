#include "syscall_table.h"

#include <string_view>

namespace uee {
namespace {

constexpr std::string_view kNames[kSyscallSlots] = {
    "UndefinedSyscall",  // 0
    "ResetEE",  // 1
    "SetGsCrt",  // 2
    "UndefinedSyscall",  // 3
    "KExit",  // 4
    "ResumeIntrDispatch",  // 5
    "_LoadExecPS2",  // 6
    "_ExecPS2",  // 7
    "UndefinedSyscall",  // 8
    "RFU009",  // 9
    "AddSbusIntcHandler",  // 10
    "RemoveSbusIntcHandler",  // 11
    "Interrupt2Iop",  // 12
    "SetVTLBRefillHandler",  // 13
    "SetVCommonHandler",  // 14
    "SetVInterruptHandler",  // 15
    "AddIntcHandler",  // 16
    "RemoveIntcHandler",  // 17
    "AddDmacHandler",  // 18
    "RemoveDmacHandler",  // 19
    "_EnableIntc",  // 20
    "_DisableIntc",  // 21
    "_EnableDmac",  // 22
    "_DisableDmac",  // 23
    "_SetAlarm",  // 24
    "_ReleaseAlarm",  // 25
    "_EnableIntc",  // 26
    "_DisableIntc",  // 27
    "_EnableDmac",  // 28
    "_DisableDmac",  // 29
    "_SetAlarm",  // 30
    "_ReleaseAlarm",  // 31
    "CreateThread",  // 32
    "DeleteThread",  // 33
    "StartThread",  // 34
    "ExitThread",  // 35
    "ExitDeleteThread",  // 36
    "TerminateThread",  // 37
    "iTerminateThread",  // 38
    "DisableDispatchThread",  // 39
    "EnableDispatchThread",  // 40
    "ChangeThreadPriority",  // 41
    "iChangeThreadPriority",  // 42
    "RotateThreadReadyQueue",  // 43
    "iRotateThreadReadyQueue",  // 44
    "ReleaseWaitThread",  // 45
    "iReleaseWaitThread",  // 46
    "GetThreadId",  // 47
    "ReferThreadStatus",  // 48
    "ReferThreadStatus",  // 49
    "SleepThread",  // 50
    "WakeupThread",  // 51
    "iWakeupThread",  // 52
    "CancelWakeupThread",  // 53
    "CancelWakeupThread",  // 54
    "SuspendThread",  // 55
    "SuspendThread",  // 56
    "ResumeThread",  // 57
    "iResumeThread",  // 58
    "RFU059",  // 59
    "SetupThread",  // 60
    "SetupHeap",  // 61
    "EndOfHeap",  // 62
    "UndefinedSyscall",  // 63
    "CreateSema",  // 64
    "DeleteSema",  // 65
    "SignalSema",  // 66
    "iSignalSema",  // 67
    "WaitSema",  // 68
    "PollSema",  // 69
    "PollSema",  // 70
    "ReferSemaStatus",  // 71
    "ReferSemaStatus",  // 72
    "iDeleteSema",  // 73
    "SetOsdConfigParam",  // 74
    "GetOsdConfigParam",  // 75
    "GetGsHParam",  // 76
    "GetGsVParam",  // 77
    "SetGsHParam",  // 78
    "SetGsVParam",  // 79
    "GetThreadCount",  // 80
    "GetSemaCount",  // 81
    "GetAlarmCount",  // 82
    "GetIntrHandlerCount",  // 83
    "UndefinedSyscall",  // 84
    "UndefinedSyscall",  // 85
    "UndefinedSyscall",  // 86
    "UndefinedSyscall",  // 87
    "UndefinedSyscall",  // 88
    "UndefinedSyscall",  // 89
    "UndefinedSyscall",  // 90
    "UndefinedSyscall",  // 91
    "EnableIntcHandler",  // 92
    "DisableIntcHandler",  // 93
    "EnableDmacHandler",  // 94
    "DisableDmacHandler",  // 95
    "sub_A0002C00",  // 96
    "sub_A00028C0",  // 97
    "sub_A0002980",  // 98
    "GetCop0",  // 99
    "FlushCache",  // 100
    "RFU105",  // 101
    "CpuConfig",  // 102
    "GetCop0",  // 103
    "FlushCache",  // 104
    "RFU105",  // 105
    "CpuConfig",  // 106
    "SifStopDma",  // 107
    "SetCPUTimerHandler",  // 108
    "SetCPUTimer",  // 109
    "SetOsdConfigParam2",  // 110
    "GetOsdConfigParam2",  // 111
    "GsGetIMR",  // 112
    "GsPutIMR",  // 113
    "SetPgifHandler",  // 114
    "SetVSyncFlag",  // 115
    "SetSyscall",  // 116
    "_print",  // 117
    "SifDmaStat",  // 118
    "SifSetDma",  // 119
    "SifSetDChain",  // 120
    "SifSetReg",  // 121
    "SifGetReg",  // 122
    "_ExecOSD",  // 123
    "UndefinedSyscall",  // 124
    "PSMode",  // 125
    "MachineType",  // 126
    "GetMemorySize",  // 127
    "GetGsDxDyOffset",  // 128
    "<null>",  // 129
    "InitTLB",  // 130
};

constexpr u32 kRomTable[kSyscallSlots] = {
    0x80001564,  //   0 UndefinedSyscall
    0x8000d9b8,  //   1 ResetEE
    0x8000bd58,  //   2 SetGsCrt
    0x80001564,  //   3 UndefinedSyscall
    0x80000d80,  //   4 KExit
    0x80002880,  //   5 ResumeIntrDispatch
    0x80005598,  //   6 _LoadExecPS2
    0x80002f80,  //   7 _ExecPS2
    0x80001564,  //   8 UndefinedSyscall
    0x80005bc8,  //   9 RFU009
    0x80001e78,  //  10 AddSbusIntcHandler
    0x80001ed0,  //  11 RemoveSbusIntcHandler
    0x80001f70,  //  12 Interrupt2Iop
    0x800009c0,  //  13 SetVTLBRefillHandler
    0x80000a00,  //  14 SetVCommonHandler
    0x80000a40,  //  15 SetVInterruptHandler
    0x80001a30,  //  16 AddIntcHandler
    0x80001a70,  //  17 RemoveIntcHandler
    0x80001c78,  //  18 AddDmacHandler
    0x80001cb8,  //  19 RemoveDmacHandler
    0x800008c0,  //  20 _EnableIntc
    0x80000900,  //  21 _DisableIntc
    0x80000940,  //  22 _EnableDmac
    0x80000980,  //  23 _DisableDmac
    0x800022d8,  //  24 _SetAlarm
    0x80002570,  //  25 _ReleaseAlarm
    0x800008c0,  //  26 _EnableIntc
    0x80000900,  //  27 _DisableIntc
    0x80000940,  //  28 _EnableDmac
    0x80000980,  //  29 _DisableDmac
    0x800022d8,  //  30 _SetAlarm
    0x80002570,  //  31 _ReleaseAlarm
    0x80003c50,  //  32 CreateThread
    0x80002fc0,  //  33 DeleteThread
    0x80003040,  //  34 StartThread
    0x800030c0,  //  35 ExitThread
    0x80003100,  //  36 ExitDeleteThread
    0x80003140,  //  37 TerminateThread
    0x80003df8,  //  38 iTerminateThread
    0x80004220,  //  39 DisableDispatchThread
    0x80004250,  //  40 EnableDispatchThread
    0x800035c0,  //  41 ChangeThreadPriority
    0x80004280,  //  42 iChangeThreadPriority
    0x800031c0,  //  43 RotateThreadReadyQueue
    0x80004380,  //  44 iRotateThreadReadyQueue
    0x80003240,  //  45 ReleaseWaitThread
    0x800043c8,  //  46 iReleaseWaitThread
    0x80004540,  //  47 GetThreadId
    0x80004550,  //  48 ReferThreadStatus
    0x80004550,  //  49 ReferThreadStatus
    0x800032c0,  //  50 SleepThread
    0x80003340,  //  51 WakeupThread
    0x800046a8,  //  52 iWakeupThread
    0x80004968,  //  53 CancelWakeupThread
    0x80004968,  //  54 CancelWakeupThread
    0x80004800,  //  55 SuspendThread
    0x80004800,  //  56 SuspendThread
    0x800033c0,  //  57 ResumeThread
    0x800048b8,  //  58 iResumeThread
    0x8000cfb0,  //  59 RFU059
    0x80005190,  //  60 SetupThread
    0x80005298,  //  61 SetupHeap
    0x800052d0,  //  62 EndOfHeap
    0x80001564,  //  63 UndefinedSyscall
    0x800049b8,  //  64 CreateSema
    0x80003540,  //  65 DeleteSema
    0x800034c0,  //  66 SignalSema
    0x80004bc0,  //  67 iSignalSema
    0x80003440,  //  68 WaitSema
    0x80004dc0,  //  69 PollSema
    0x80004dc0,  //  70 PollSema
    0x80004df8,  //  71 ReferSemaStatus
    0x80004df8,  //  72 ReferSemaStatus
    0x80004a40,  //  73 iDeleteSema
    0x8000d470,  //  74 SetOsdConfigParam
    0x8000d3c0,  //  75 GetOsdConfigParam
    0x8000cd18,  //  76 GetGsHParam
    0x8000cf58,  //  77 GetGsVParam
    0x8000cfd0,  //  78 SetGsHParam
    0x8000d260,  //  79 SetGsVParam
    0x800049a8,  //  80 GetThreadCount
    0x80004e50,  //  81 GetSemaCount
    0x800027e8,  //  82 GetAlarmCount
    0x80002040,  //  83 GetIntrHandlerCount
    0x80001564,  //  84 UndefinedSyscall
    0x80001564,  //  85 UndefinedSyscall
    0x80001564,  //  86 UndefinedSyscall
    0x80001564,  //  87 UndefinedSyscall
    0x80001564,  //  88 UndefinedSyscall
    0x80001564,  //  89 UndefinedSyscall
    0x80001564,  //  90 UndefinedSyscall
    0x80001564,  //  91 UndefinedSyscall
    0x80001fa0,  //  92 EnableIntcHandler
    0x80001fc8,  //  93 DisableIntcHandler
    0x80001ff0,  //  94 EnableDmacHandler
    0x80002018,  //  95 DisableDmacHandler
    0xa0002c00,  //  96 sub_A0002C00
    0xa00028c0,  //  97 sub_A00028C0
    0xa0002980,  //  98 sub_A0002980
    0x80002c40,  //  99 GetCop0
    0x80002a40,  // 100 FlushCache
    0x80002b40,  // 101 RFU105
    0x80000a80,  // 102 CpuConfig
    0x80002c40,  // 103 GetCop0
    0x80002a40,  // 104 FlushCache
    0x80002b40,  // 105 RFU105
    0x80000a80,  // 106 CpuConfig
    0x80006378,  // 107 SifStopDma
    0x800007c0,  // 108 SetCPUTimerHandler
    0x80000800,  // 109 SetCPUTimer
    0x8000d350,  // 110 SetOsdConfigParam2
    0x8000d2b8,  // 111 GetOsdConfigParam2
    0x80000d00,  // 112 GsGetIMR
    0x80000d40,  // 113 GsPutIMR
    0x80000740,  // 114 SetPgifHandler
    0x80001588,  // 115 SetVSyncFlag
    0x800006c0,  // 116 SetSyscall
    0x800074c8,  // 117 _print
    0x80006950,  // 118 SifDmaStat
    0x80006798,  // 119 SifSetDma
    0x80006340,  // 120 SifSetDChain
    0x80006c00,  // 121 SifSetReg
    0x80006cb8,  // 122 SifGetReg
    0x80005988,  // 123 _ExecOSD
    0x80001564,  // 124 UndefinedSyscall
    0x80000b80,  // 125 PSMode
    0x80000bc0,  // 126 MachineType
    0x80000c40,  // 127 GetMemorySize
    0x8000cca0,  // 128 GetGsDxDyOffset
    0x00000000,  // 129 <null>
    0x80005c30,  // 130 InitTLB
};

}  // namespace

// KERNEL: k_SyscallTable @ 0x80014f40
void installSyscallTable(Memory& memory) {
    for (s32 number = 0; number < kSyscallSlots; ++number) {
        memory.write32(kSyscallTableBase + static_cast<u32>(number) * 4, kRomTable[number]);
    }
}

u32 readSyscallHandler(Memory& memory, s32 number) {
    return memory.read32(kSyscallTableBase + static_cast<u32>(number) * 4);
}

// KERNEL: SetSyscall @ 0x800006c0
void writeSyscallHandler(Memory& memory, s32 number, u32 handler) {
    memory.write32(kSyscallTableBase + static_cast<u32>(number) * 4, handler);
}

// The i entries run inside a handler, with no saved context to switch away
// from, so they leave the dispatch to the interrupt return.
std::string_view nameOfSyscall(s32 number) {
    const s32 slot = number < 0 ? -number : number;
    return slot < kSyscallSlots ? kNames[slot] : std::string_view{};
}

bool isInterruptSyscall(s32 number) {
    const std::string_view name = nameOfSyscall(number);
    // TODO: use a lookup table instead
    return name.size() > 1 && name[0] == 'i' && name[1] >= 'A' && name[1] <= 'Z';
}

u32 getDefaultSyscallHandler(s32 number) {
    const s32 slot = number < 0 ? -number : number;
    return slot < kSyscallSlots ? kRomTable[slot] : 0;
}

}  // namespace uee
