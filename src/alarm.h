#pragma once

#include "types.h"

#include <array>
#include <vector>

namespace uee {

constexpr usize kAlarmCount = 64;

// Mirrors EeAlarm. Times are Timer 3 H-SYNC ticks and wrap at 16 bits.
struct Alarm {
    u32 callback = 0;
    u32 argument = 0;
    u32 globalPointer = 0;
    u16 start = 0;
    u16 target = 0;
};

struct ExpiredAlarm {
    s32 id = 0;
    u16 target = 0;
    u32 callback = 0;
    u32 argument = 0;
};

// The 64 alarm slots, the free mask, and the active list kept sorted by target.
class AlarmTable {
public:
    s32 set(u16 ticks, u32 callback, u32 argument, u32 globalPointer, u16 now);
    // Elapsed ticks since the alarm was set, or -1 when the slot is not in use.
    s32 release(s32 id, u16 now);

    // Removes and returns every alarm whose target has been reached, oldest first.
    std::vector<ExpiredAlarm> takeExpired(u16 now);

    s32 count() const { return static_cast<s32>(mActive.size()); }
    bool isActive(s32 id) const;
    // The target the hardware compare should be programmed to.
    bool nextTarget(u16& target) const;

private:
    s32 allocate();

    std::array<Alarm, kAlarmCount> mAlarms;
    std::vector<u8> mActive;
    u64 mFreeMask = 0;
};

}  // namespace uee
