#include "timers.h"

namespace uee {
namespace {

constexpr u32 kCountEnable = 1u << 7;
constexpr u32 kZeroReturn = 1u << 6;
constexpr u32 kEqualFlag = 1u << 10;
constexpr u32 kOverflowFlag = 1u << 11;

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

// The modes InitTimers programs at boot. T0 and T1 are left stopped; T2 and T3
// count, and T3 is the H-SYNC source alarms are scheduled against.
// KERNEL: InitTimers @ 0x8000e648
Timers::Timers() {
    mTimers[0].mode = 3072;
    mTimers[1].mode = 3072;
    mTimers[2].mode = 3200;
    mTimers[3].mode = 3203;
    for (Timer& timer : mTimers) {
        timer.count = 0;
        timer.comp = 0xFFFF;
    }
}

bool Timers::advanceTo(u64 retiredInstructions) {
    const u64 ticks = retiredInstructions / kInstructionsPerHsync;
    if (ticks == mLastTick) {
        return false;
    }
    const u32 elapsed = static_cast<u32>(ticks - mLastTick);
    mLastTick = ticks;

    for (Timer& timer : mTimers) {
        if ((timer.mode & kCountEnable) == 0) {
            continue;
        }
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
    return true;
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
    Timer& timer = mTimers[slot->timer];
    switch (slot->field) {
        case 0x00: timer.count = value & 0xFFFF; break;
        // The equal and overflow flags are write-one-to-clear, so a mode write
        // clears exactly the flags it sets.
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
