#include "alarm.h"

#include <algorithm>

namespace uee {

// KERNEL: the k_AlarmFreeMask scan in _SetAlarm @ 0x800022d8
s32 AlarmTable::allocate() {
    for (usize i = 0; i < kAlarmCount; ++i) {
        const u64 bit = 1ull << i;
        if ((mFreeMask & bit) == 0) {
            mFreeMask |= bit;
            return static_cast<s32>(i);
        }
    }
    return -1;
}

bool AlarmTable::isActive(s32 id) const {
    if (id < 0 || static_cast<usize>(id) >= kAlarmCount) {
        return false;
    }
    return (mFreeMask & (1ull << id)) != 0;
}

// KERNEL: _SetAlarm @ 0x800022d8
s32 AlarmTable::set(u16 ticks, u32 callback, u32 argument, u32 gp, u16 now) {
    const s32 id = allocate();
    if (id < 0) {
        return -1;
    }

    const u16 target = static_cast<u16>(static_cast<u32>(now) + ticks);
    Alarm& alarm = mAlarms[static_cast<usize>(id)];
    alarm.callback = callback;
    alarm.argument = argument;
    alarm.gp = gp;
    alarm.start = now;
    alarm.target = target;

    // A target at or below now has wrapped, so it sorts after those that have not.
    usize at = 0;
    if (now >= target) {
        while (at < mActive.size() && mAlarms[mActive[at]].target >= now) {
            ++at;
        }
    }
    while (at < mActive.size() && target >= mAlarms[mActive[at]].target) {
        ++at;
    }
    mActive.insert(mActive.begin() + static_cast<isize>(at), static_cast<u8>(id));
    return id;
}

// KERNEL: _ReleaseAlarm @ 0x80002570
s32 AlarmTable::release(s32 id, u16 now) {
    if (!isActive(id)) {
        return -1;
    }
    const Alarm& alarm = mAlarms[static_cast<usize>(id)];
    mFreeMask &= ~(1ull << id);
    mActive.erase(std::remove(mActive.begin(), mActive.end(), static_cast<u8>(id)), mActive.end());

    const u32 elapsed = (now >= alarm.start) ? static_cast<u32>(now) - alarm.start
                                             : static_cast<u32>(now) + 0x10000 - alarm.start;
    return static_cast<s32>(elapsed);
}

// KERNEL: AlarmTimerHandler @ 0x80002650
std::vector<ExpiredAlarm> AlarmTable::takeExpired(u16 now) {
    std::vector<ExpiredAlarm> expired;
    while (!mActive.empty()) {
        const usize id = mActive.front();
        const Alarm& alarm = mAlarms[id];
        const u16 requested = static_cast<u16>(alarm.target - alarm.start);
        const u16 elapsed = static_cast<u16>(now - alarm.start);
        if (elapsed < requested) {
            break;
        }
        expired.push_back(ExpiredAlarm{static_cast<s32>(id), alarm.target, alarm.callback,
                                     alarm.argument});
        mFreeMask &= ~(1ull << id);
        mActive.erase(mActive.begin());
    }
    return expired;
}

bool AlarmTable::nextTarget(u16& target) const {
    if (mActive.empty()) {
        return false;
    }
    target = mAlarms[mActive.front()].target;
    return true;
}

}  // namespace uee
