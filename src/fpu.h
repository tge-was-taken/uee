#pragma once

#include "types.h"

namespace uee {

// FCR0 reads the implementation and revision; every register below 16 is an
// alias for it and every register above is an alias for FCR31.
struct Fpu {
    static constexpr u32 kImplementation = 0x00002E30;

    static constexpr u32 kCompareBit = 0x00800000u;

    // Rounding is always toward zero and denormals always flush, so the two
    // bits that would say otherwise read as one and ignore writes.
    static constexpr u32 kHardwired = 0x01000001u;
    static constexpr u32 kWritable = 0x0083C078u;

    static constexpr u32 kStickyUnderflow = 1u << 3;
    static constexpr u32 kStickyOverflow = 1u << 4;
    static constexpr u32 kStickyDivideByZero = 1u << 5;
    static constexpr u32 kStickyInvalid = 1u << 6;
    static constexpr u32 kUnderflow = 1u << 14;
    static constexpr u32 kOverflow = 1u << 15;
    static constexpr u32 kDivideByZero = 1u << 16;
    static constexpr u32 kInvalid = 1u << 17;
    static constexpr u32 kCauseMask =
        kUnderflow | kOverflow | kDivideByZero | kInvalid;

    u32 fpr[32]{};
    u32 acc = 0;
    u32 fcr31 = kHardwired;

    u32 readControl(u32 index) const {
        return (index < 16) ? kImplementation : fcr31;
    }

    void writeControl(u32 index, u32 value) {
        if (index >= 16) {
            fcr31 = (value & kWritable) | kHardwired;
        }
    }

    // A cause bit reports the operation that just ran; its sticky twin four
    // places down accumulates until the guest clears it.
    void raise(u32 causeBits) {
        fcr31 = (fcr31 & ~kCauseMask) | causeBits | (causeBits >> 11);
    }
};

}  // namespace uee
