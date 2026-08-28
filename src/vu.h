#pragma once

#include "r5900.h"
#include "types.h"

namespace uee {

// The integer file is addressed 0 to 31: below 16 the 16-bit registers, above
// it the control registers. cfc2 and ctc2 reach all of them.
struct Vu {
    static constexpr u32 kStatus = 16;
    static constexpr u32 kMac = 17;
    static constexpr u32 kClip = 18;
    static constexpr u32 kR = 20;
    static constexpr u32 kI = 21;
    static constexpr u32 kQ = 22;
    static constexpr u32 kP = 23;
    static constexpr u32 kTpc = 26;
    static constexpr u32 kCmsar0 = 27;
    static constexpr u32 kCmsar1 = 31;
    static constexpr u32 kIntegerMask = 0x0000FFFF;

    Reg128 vf[32]{};
    u32 vi[32]{};
    Reg128 acc{};
    // What the clipping unit has worked out so far. The register a program can
    // read lags it, so the two are kept apart.
    u32 clip = 0;

    // the destination field runs from bit 3 for x down to bit 0 for w
    static constexpr bool writes(u32 dest, s32 lane) {
        return (dest & (8u >> lane)) != 0;
    }

    // VF00 reads as (0, 0, 0, 1) and ignores writes
    Reg128 readVf(u32 index) const;
    void writeVf(u32 index, u32 dest, const u32 result[4]);
    void writeAcc(u32 dest, const u32 result[4]);

    // VI00 reads as zero and ignores writes; only the control registers are
    // wider than 16 bits
    u32 readVi(u32 index) const;
    void writeVi(u32 index, u32 value);

    // What a write to each control register keeps. The integer registers hold
    // sixteen bits, and the ones above them each keep their own fields.
    static u32 controlMask(u32 index);
    void writeControl(u32 index, u32 value);
};

}  // namespace uee
