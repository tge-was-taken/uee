#include "timers.h"

#include <algorithm>
#include <utility>

namespace uee {
namespace {

constexpr u32 kClockSelectMask = 3u;
constexpr u32 kHblankClock = 3u;
constexpr u32 kCountEnable = 1u << 7;
constexpr u32 kZeroReturn = 1u << 6;
constexpr u32 kEqualFlag = 1u << 10;
constexpr u32 kOverflowFlag = 1u << 11;

constexpr u64 kInstructionsPerTick[4] = {
    kInstructionsPerBusCycle,
    kInstructionsPerBusCycle * 16,
    kInstructionsPerBusCycle * 256,
    kInstructionsPerHsync,
};

struct Slot {
    usize timer;
    u32 field;
};

std::optional<Slot> decode(u32 address) {
    const u32 offset = address - kTimerBase;
    const usize timer = offset / kTimerStride;
    if (timer >= kTimerCount) {
        return std::nullopt;
    }
    const u32 field = offset % kTimerStride;
    if (field > 0x30 || (field % 0x10) != 0) {
        return std::nullopt;
    }
    return Slot{timer, field};
}

}  // namespace

// KERNEL: InitTimers @ 0x8000e648
Timers::Timers() {
    mTimers[0].mode = 0;
    mTimers[1].mode = 0;
    mTimers[2].mode = kCountEnable;
    mTimers[3].mode = kCountEnable | kHblankClock;
    for (Timer& timer : mTimers) {
        timer.count = 0;
        timer.comp = 0xFFFF;
    }
}

bool Timers::runTicks(u64 executed) {
    u64 next = executed + kInstructionsPerHsync;
    for (Timer& timer : mTimers) {
        // Check for stopped timer
        if ((timer.mode & kCountEnable) == 0) {
            timer.counted = executed;
            continue;
        }

        const u64 period = kInstructionsPerTick[timer.mode & kClockSelectMask];
        const u64 ticks = (executed - timer.counted) / period;
        timer.counted += ticks * period;
        next = std::min(next, timer.counted + period);
        if (ticks == 0) {
            continue;
        }

        const u32 elapsed = static_cast<u32>(std::min<u64>(ticks, 0x10000));
        const u64 raised = static_cast<u64>(timer.count) + elapsed;
        if (raised > 0xFFFF) {
            timer.mode |= kOverflowFlag;
        }
        if ((timer.mode & kZeroReturn) != 0 && raised >= timer.comp) {
            timer.mode |= kEqualFlag;
            timer.count = static_cast<u32>(raised % (timer.comp + 1));
        } else {
            if (timer.count < timer.comp && raised >= timer.comp) {
                timer.mode |= kEqualFlag;
            }
            timer.count = static_cast<u32>(raised & 0xFFFF);
        }
    }

    // The scanline the kernel schedules against is counted here rather than
    // read off timer 3, so a guest reprogramming that timer cannot stop it.
    const u64 scanlines = executed / kInstructionsPerHsync;
    const bool passed = scanlines != mScanlines;
    mScanlines = scanlines;
    mNextTick = std::min(next, (scanlines + 1) * kInstructionsPerHsync);
    return passed;
}

std::optional<u32> Timers::read(u32 address) const {
    const std::optional<Slot> slot = decode(address);
    if (!slot) {
        return std::nullopt;
    }
    const Timer& timer = mTimers[slot->timer];
    switch (slot->field) {
        case 0x00: return timer.count;
        case 0x10: return timer.mode;
        case 0x20: return timer.comp;
        default: return timer.hold;
    }
}

bool Timers::write(u32 address, u32 value) {
    const std::optional<Slot> slot = decode(address);
    if (!slot) {
        return false;
    }
    // A mode write can change how fast the timer runs, so when it next needs
    // looking at is no longer known.
    mNextTick = 0;
    Timer& timer = mTimers[slot->timer];
    switch (slot->field) {
        case 0x00: timer.count = value & 0xFFFF; break;
        // A mode write clears the flags it sets.
        case 0x10: {
            const u32 cleared = timer.mode & ~(value & (kEqualFlag | kOverflowFlag));
            timer.mode = (value & ~(kEqualFlag | kOverflowFlag)) | (cleared & (kEqualFlag | kOverflowFlag));
            break;
        }
        case 0x20: timer.comp = value & 0xFFFF; break;
        default: timer.hold = value; break;
    }
    return true;
}

}  // namespace uee
