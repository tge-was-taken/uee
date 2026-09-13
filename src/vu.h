#pragma once

#include "r5900.h"
#include "single.h"
#include "types.h"

namespace uee {

class VuPrograms;

// The integer file is addressed 0 to 31: below 16 the 16-bit registers, above
// it the control registers. cfc2 and ctc2 reach all of them.
struct Vu {
    // What the divider produced and the two conditions it reports.
    struct Quotient {
        u32 value = 0;
        bool invalid = false;
        bool byZero = false;
    };

    static constexpr u32 kStatus = 16;
    static constexpr u32 kMac = 17;
    static constexpr u32 kClip = 18;
    static constexpr u32 kR = 20;
    static constexpr u32 kI = 21;
    static constexpr u32 kQ = 22;
    static constexpr u32 kP = 23;
    static constexpr u32 kTpc = 26;
    static constexpr u32 kFbrst = 28;
    static constexpr u32 kVpuStat = 29;
    static constexpr u32 kUnit0Busy = 1u << 0;
    static constexpr u32 kUnit0Stopped = 1u << 3;
    static constexpr u32 kUnit1Stopped = 1u << 11;
    static constexpr u32 kCmsar0 = 27;
    static constexpr u32 kCmsar1 = 31;
    static constexpr u32 kIntegerMask = 0x0000FFFF;

    Reg128 vf[32]{};
    u32 vi[32]{};
    // The rom leaves every lane of the accumulator set, which a program that
    // reads it without writing it first finds there.
    Reg128 acc{~0ull, ~0ull};
    // Saturating loses how far past the range a sum went, so the next accumulate
    // saturates again. The rom leaves the accumulator saturated.
    bool accSaturated[4]{true, true, true, true};
    Reg128 beforeCall[32]{};
    bool hasBeforeCall = false;
    struct Flags {
        u32 status;
        u32 mac;
    };
    Flags seenFlags{};
    Flags stagedLive{};
    Flags stagedFull{};
    u32 keptSticky = 0;

    u32 pendingQuotient = 0;
    u32 quotientDelay = 0;
    u32 statusSeen = 0;
    u32 stopped = 0;
    bool started = false;
    // A program that never reached its end bit is still running, and the
    // interface waits rather than starting a second one over it.
    bool microRunning = false;
    // Where micro programs go to find a translation. Every start comes through
    // the unit's own state, including one the processor makes with vcallms.
    VuPrograms* programs = nullptr;
    // What the clipping unit has worked out so far. The register a program can
    // read lags it, so the two are kept apart.
    u32 clip = 0;

    // the destination field runs from bit 3 for x down to bit 0 for w
    static constexpr bool writes(u32 dest, s32 lane) {
        return (dest & (8u >> lane)) != 0;
    }

    // VF00 reads as (0, 0, 0, 1) and ignores writes.
    Reg128 readVf(u32 index) const {
        return (index == 0) ? Reg128{0, 0x3F800000ull << 32} : vf[index];
    }

    void writeVf(u32 index, u32 dest, const u32 result[4]) {
        if (index != 0) {
            merge(vf[index], dest, result);
        }
    }

    void writeAcc(u32 dest, const u32 result[4], const bool saturated[4]) {
        merge(acc, dest, result);
        for (s32 i = 0; i < 4; ++i) {
            if (writes(dest, i)) {
                accSaturated[i] = saturated[i];
            }
        }
    }

    // A microprogram is still running when the instruction that started it
    // returns, and a plain read of the status is a step behind the unit itself.
    u32 takeStatus() {
        const u32 seen = statusSeen;
        statusSeen = started ? kUnit0Busy : 0u;
        started = false;
        return seen | stopped;
    }

    u32 readStatus() const { return (started ? kUnit0Busy : statusSeen) | stopped; }

    // The divider hands its answer over some instructions after the one that
    // asked for it, and a read before then sees what the register held.
    static constexpr u32 kQuotientDelay = 6;

    // Flags are readable one instruction later and their sticky copies one after
    // that, except the two the divider owns.
    static constexpr u32 kStickyMask = 0x3C0u;

    void stageFlags(u32 sticky) {
        stagedLive = {(vi[kStatus] & ~kStickyMask) | sticky, vi[kMac]};
        stagedFull = {vi[kStatus], vi[kMac]};
    }

    void advanceFlags() {
        seenFlags = stagedLive;
        stagedLive = stagedFull;
    }

    // Around each macro instruction: begin publishes what the one before left,
    // end hands over what this one produced.
    void beginMacro() {
        advanceQuotient();
        keptSticky = vi[kStatus] & kStickyMask;
        advanceFlags();
    }

    void endMacro() { stageFlags(keptSticky); }

    void syncFlags() {
        seenFlags = {vi[kStatus], vi[kMac]};
        stagedLive = seenFlags;
        stagedFull = seenFlags;
    }

    u32 readSeenStatus() const { return seenFlags.status; }
    u32 readSeenMac() const { return seenFlags.mac; }

    void delayQuotient(u32 previous) {
        pendingQuotient = vi[kQ];
        vi[kQ] = previous;
        quotientDelay = kQuotientDelay;
    }

    void advanceQuotient() {
        if (quotientDelay != 0 && --quotientDelay == 0) {
            vi[kQ] = pendingQuotient;
        }
    }

    void syncQuotient() {
        if (quotientDelay != 0) {
            vi[kQ] = pendingQuotient;
            quotientDelay = 0;
        }
    }

    // A program that has not finished has not written the registers a plain
    // transfer reads, so the first one after a call still sees what was there.
    void keepBeforeCall() {
        for (u32 i = 0; i < 32; ++i) {
            beforeCall[i] = vf[i];
        }
        hasBeforeCall = true;
    }

    Reg128 takeVf(u32 index) {
        if (!hasBeforeCall) {
            return readVf(index);
        }
        hasBeforeCall = false;
        return (index == 0) ? Reg128{0, 0x3F800000ull << 32} : beforeCall[index];
    }

    // Which of the two force break bits each unit answers is not the same one:
    // resetting the first unit is what marks it, and breaking the second.
    void applyForceBreak(u32 value) {
        // A reset stops whatever the unit was running, which is what lets a
        // guest recover a program that never reached its end bit.
        if ((value & 0x0003u) != 0 || (value & 0x0300u) != 0) {
            microRunning = false;
        }
        if ((value & 0x0001u) != 0) {
            stopped |= kUnit0Stopped;
        }
        if ((value & 0x0002u) != 0) {
            stopped &= ~kUnit0Stopped;
        }
        if ((value & 0x0200u) != 0) {
            stopped |= kUnit1Stopped;
        }
        if ((value & 0x0100u) != 0) {
            stopped &= ~kUnit1Stopped;
        }
    }

    void loadAcc(const Reg128& value) {
        acc = value;
        for (bool& lane : accSaturated) {
            lane = false;
        }
    }

    // Four bits per lane, grouped: zero, sign, underflow, overflow, with x at
    // the top of each group.
    static constexpr u32 kUnderflow = 1u << 8;
    static constexpr u32 kOverflow = 1u << 12;

    static constexpr u32 kInvalid = 1u << 4;
    static constexpr u32 kDivideByZero = 1u << 5;
    static constexpr u32 kInvalidSticky = 1u << 10;
    static constexpr u32 kDivideByZeroSticky = 1u << 11;

    u32 getMacBits(u32 dest, const u32 result[4], const u32 extra[4]) const {
        u32 mac = 0;
        for (s32 i = 0; i < 4; ++i) {
            if (!writes(dest, i)) {
                continue;
            }
            const u32 lane = 8u >> i;
            if ((result[i] & 0x7FFFFFFFu) == 0) {
                mac |= lane;
            }
            if ((result[i] & 0x80000000u) != 0) {
                mac |= lane << 4;
            }
            if ((extra[i] & kUnderflow) != 0) {
                mac |= lane << 8;
            }
            if ((extra[i] & kOverflow) != 0) {
                mac |= lane << 12;
            }
        }
        return mac;
    }

    static u32 collapseMac(u32 mac) {
        u32 live = 0;
        for (s32 kind = 0; kind < 4; ++kind) {
            if ((mac & (0xFu << (kind * 4))) != 0) {
                live |= 1u << kind;
            }
        }
        return live;
    }

    void setFlags(u32 dest, const u32 result[4], const u32 extra[4]) {
        const u32 mac = getMacBits(dest, result, extra);
        vi[kMac] = mac;
        const u32 live = collapseMac(mac);
        // The sticky copies sit six places up and only ever gain bits.
        vi[kStatus] = (vi[kStatus] & ~0xFu) | live | (live << 6);
    }

    // A multiply feeding an accumulate passes through the flag logic on its own
    // before the sum replaces the visible result.
    void addSticky(u32 dest, const u32 result[4], const u32 extra[4]) {
        vi[kStatus] |= collapseMac(getMacBits(dest, result, extra)) << 6;
    }

    // The two conditions the divider reports live in their own bits and are
    // replaced each time, unlike the four the adder shares with the MAC register.
    void setDivideFlags(bool invalid, bool byZero) {
        u32 status = vi[kStatus] & ~(kInvalid | kDivideByZero);
        if (invalid) {
            status |= kInvalid | kInvalidSticky;
        }
        if (byZero) {
            status |= kDivideByZero | kDivideByZeroSticky;
        }
        vi[kStatus] = status;
    }

private:
    // Writing every lane leaves nothing of the old value to keep.
    static void merge(Reg128& target, u32 dest, const u32 result[4]) {
        if (dest == 0xFu) {
            target = lanes(result[0], result[1], result[2], result[3]);
            return;
        }
        for (s32 i = 0; i < 4; ++i) {
            if (writes(dest, i)) {
                target.setWord(i, result[i]);
            }
        }
    }

public:
    // VI00 reads as zero and ignores writes; only the control registers are
    // wider than 16 bits
    u32 readVi(u32 index) const { return index == 0 ? 0 : vi[index]; }
    void writeVi(u32 index, u32 value) {
        if (index != 0) {
            vi[index] = index < 16 ? (value & 0xFFFFu) : value;
        }
    }

    // What a write to each control register keeps. The integer registers hold
    // sixteen bits, and the ones above them each keep their own fields.
    static u32 controlMask(u32 index);
    void writeControl(u32 index, u32 value);
};

}  // namespace uee
