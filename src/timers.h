#pragma once

#include "types.h"

#include <array>
#include <optional>

namespace uee {

constexpr u32 kTimerBase = 0x10000000;
constexpr u32 kTimerStride = 0x800;
constexpr usize kTimerCount = 4;

// NTSC H-SYNC is 15.734 kHz against a 294.912 MHz core, and the runtime retires
// one instruction per cycle, so this many instructions make one tick.
constexpr u64 kInstructionsPerHsync = 18742;

// NTSC draws 262.5 lines per field at 59.94 Hz, so a field is this many H-SYNC
// ticks and VBLANK starts once the visible lines are done.
constexpr u32 kHsyncPerField = 263;
constexpr u32 kVisibleLines = 240;

struct Timer {
    u32 count = 0;
    u32 mode = 0;
    u32 comp = 0xFFFF;
    u32 hold = 0;
};

// The four EE timers plus the instruction-driven clock that advances them.
// Timer 3 counts H-SYNC and is the time base alarms are scheduled against.
class Timers {
public:
    Timers();

    // True when at least one H-SYNC tick elapsed, so callers can skip work.
    bool advanceTo(u64 retiredInstructions);

    u16 hsyncCount() const { return static_cast<u16>(mTimers[3].count); }

    std::optional<u32> read(u32 address) const;
    bool write(u32 address, u32 value);

    Timer& at(usize index) { return mTimers[index]; }
    const Timer& at(usize index) const { return mTimers[index]; }

private:
    std::array<Timer, kTimerCount> mTimers;
    u64 mLastTick = 0;
};

}  // namespace uee
