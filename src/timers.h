#pragma once

#include "types.h"

#include <array>
#include <optional>

namespace uee {

constexpr u32 kTimerBase = 0x10000000;
constexpr u32 kTimerStride = 0x800;
constexpr usize kTimerCount = 4;

constexpr u64 kCoreClockHz = 294'912'000;
constexpr u64 kBusClockHz = 147'456'000;
constexpr u64 kIopClockHz = 36'864'000;
constexpr double kFieldsPerSecond = 59.94;
constexpr double kLinesPerField = 262.5;

constexpr u64 kInstructionsPerSecond = kCoreClockHz;
constexpr u64 kInstructionsPerBusCycle = kInstructionsPerSecond / kBusClockHz;
constexpr u64 kInstructionsPerHsync =
    static_cast<u64>(kInstructionsPerSecond / (kFieldsPerSecond * kLinesPerField));

constexpr u64 kIopInstructionsPerHsync = kInstructionsPerHsync * kIopClockHz / kCoreClockHz;

constexpr u32 kHsyncPerField = 263;
constexpr u32 kVisibleLines = 240;

struct Timer {
    u32 count = 0;
    u32 mode = 0;
    u32 comp = 0xFFFF;
    u32 hold = 0;
    // A count rather than a tick number, so a rate change carries the
    // remainder over.
    u64 counted = 0;
};

// The four EE timers plus the instruction-driven clock that advances them.
// Timer 3 counts H-SYNC and is the time base alarms are scheduled against.
class Timers {
public:
    Timers();

    // Almost always no, so only the tick itself costs a call.
    bool advanceTo(u64 executed) {
        return executed >= mNextTick && runTicks(executed);
    }

    u16 hsyncCount() const { return static_cast<u16>(mTimers[3].count); }

    std::optional<u32> read(u32 address) const;
    bool write(u32 address, u32 value);

    Timer& at(usize index) { return mTimers[index]; }
    const Timer& at(usize index) const { return mTimers[index]; }

private:
    bool runTicks(u64 executed);

    std::array<Timer, kTimerCount> mTimers;
    u64 mScanlines = 0;
    u64 mNextTick = 0;
};

}  // namespace uee
