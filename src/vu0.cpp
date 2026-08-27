#include "vu0.h"

namespace uee {

Reg128 Vu0::readVf(u32 index) const {
    return (index == 0) ? Reg128{0, 0x3F800000ull << 32} : vf[index];
}

void Vu0::writeVf(u32 index, u32 dest, const u32 result[4]) {
    if (index == 0) {
        return;
    }
    for (s32 i = 0; i < 4; ++i) {
        if (writes(dest, i)) {
            vf[index].setWord(i, result[i]);
        }
    }
}

void Vu0::writeAcc(u32 dest, const u32 result[4]) {
    for (s32 i = 0; i < 4; ++i) {
        if (writes(dest, i)) {
            acc.setWord(i, result[i]);
        }
    }
}

u32 Vu0::readVi(u32 index) const {
    return (index == 0) ? 0 : vi[index];
}

void Vu0::writeVi(u32 index, u32 value) {
    if (index != 0) {
        vi[index] = (index < 16) ? (value & 0xFFFFu) : value;
    }
}

}  // namespace uee
