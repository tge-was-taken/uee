#include "vu.h"

namespace uee {

Reg128 Vu::readVf(u32 index) const {
    return (index == 0) ? Reg128{0, 0x3F800000ull << 32} : vf[index];
}

void Vu::writeVf(u32 index, u32 dest, const u32 result[4]) {
    if (index == 0) {
        return;
    }
    for (s32 i = 0; i < 4; ++i) {
        if (writes(dest, i)) {
            vf[index].setWord(i, result[i]);
        }
    }
}

void Vu::writeAcc(u32 dest, const u32 result[4]) {
    for (s32 i = 0; i < 4; ++i) {
        if (writes(dest, i)) {
            acc.setWord(i, result[i]);
        }
    }
}

u32 Vu::readVi(u32 index) const {
    return (index == 0) ? 0 : vi[index];
}

void Vu::writeVi(u32 index, u32 value) {
    if (index != 0) {
        vi[index] = (index < 16) ? (value & 0xFFFFu) : value;
    }
}

// Writing all ones to each register and reading it back is what these came
// from, so a register that keeps nothing reads as zero.
u32 Vu::controlMask(u32 index) {
    if (index < 16) {
        return index == 0 ? 0u : kIntegerMask;
    }
    // What each control register keeps of a write, from writing all ones and
    // reading back. The reserved ones still hold a few bits.
    static constexpr u32 kMasks[] = {
        0x00000FC0, 0x00000000, 0x00FFFFFF, 0x00002E30, 0x007FFFFF, 0xFFFFFFFF,
        0xFFFFFFFF, 0x00000000, 0x00000C0C, 0x00000000, kIntegerMask, kIntegerMask,
        0x00000000, 0x00000000, 0x00000000, kIntegerMask,
    };
    return kMasks[index - 16];
}

void Vu::writeControl(u32 index, u32 value) {
    vi[index] = value & controlMask(index);
    // The clipping unit's own running value is what the next clip builds on, so
    // clearing the register a guest reads has to clear that too.
    if (index == kClip) {
        clip = vi[index];
    }
}

}  // namespace uee
