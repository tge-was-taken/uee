#pragma once

#include "r3000a.h"
#include "types.h"

#include <string_view>

namespace uee {

namespace iopstatus {
constexpr u32 kRun = 0x01;
constexpr u32 kReady = 0x02;
constexpr u32 kWait = 0x04;
constexpr u32 kSuspend = 0x08;
constexpr u32 kDormant = 0x10;
}  // namespace iopstatus

enum class IopWait : u32 {
    None = 0,
    Sleep = 1,
    Semaphore = 2,
    EventFlag = 3,
    Vblank = 5,
    Delay = 7,
};

constexpr std::string_view getWaitName(IopWait wait) {
    switch (wait) {
        case IopWait::None: return "none";
        case IopWait::Sleep: return "sleep";
        case IopWait::Semaphore: return "sema";
        case IopWait::EventFlag: return "event";
        case IopWait::Vblank: return "vblank";
        case IopWait::Delay: return "delay";
    }
    return "?";
}

struct IopThread {
    bool allocated = false;
    u32 sequence = 0;
    u32 status = 0;
    u32 attr = 0;
    u32 option = 0;
    u32 entry = 0;
    u32 stack = 0;
    u32 stackSize = 0;
    s32 initialPriority = 0;
    s32 currentPriority = 0;
    s32 wakeupCount = 0;
    IopWait wait = IopWait::None;
    u32 waitId = 0;
    u32 waitPattern = 0;
    u32 waitMode = 0;
    u32 resultAddress = 0;
    u64 waitDeadline = 0;
    // The RPC queue sceSifRpcLoop serves, the server whose handler is running,
    // and where the thread goes once the reply is sent.
    u32 rpcQueue = 0;
    u32 rpcServer = 0;
    u32 rpcResume = 0;
    // Asleep in sceSifBindRpc or sceSifCallRpc until the EE answers.
    bool awaitingEe = false;

    R3000A saved;
};

// TIMEMANI: AllocHardTimer @ 0x90 hands out six, and GetHardTimerIntrCode @ 0x370
// reads the index back out of the top nibble as (timid >> 28) - 1.
constexpr u32 kHardTimers = 6;

struct IopHardTimer {
    bool allocated = false;
    bool running = false;
    u32 handler = 0;
    u32 argument = 0;
    u32 gp = 0;
    u32 compare = 0;
    u64 nextFire = 0;
};

// A timer or hardware handler runs outside any thread, as an interrupt does.
struct IopHandlerCall {
    u32 handler = 0;
    u32 argument = 0;
    u32 gp = 0;
};

struct IopSemaphore {
    bool allocated = false;
    u32 sequence = 0;
    u32 attr = 0;
    u32 option = 0;
    s32 initial = 0;
    s32 count = 0;
    s32 maxCount = 0;
};

struct IopEventFlag {
    bool allocated = false;
    u32 sequence = 0;
    u32 attr = 0;
    u32 option = 0;
    u32 initialBits = 0;
    u32 bits = 0;
};

}  // namespace uee
