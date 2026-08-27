#pragma once

#include "types.h"

namespace uee {

struct Fpu {
    static constexpr u32 kCompareBit = 0x00800000u;

    u32 fpr[32]{};
    u32 acc = 0;
    u32 fcr31 = 0;
};

}  // namespace uee
