#pragma once

#include "r3000a.h"
#include "types.h"

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
    RpcQueue = 4,
    Vblank = 5,
    RpcReply = 6,
};

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

    R3000A saved;
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
