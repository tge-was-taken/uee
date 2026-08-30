#pragma once

#include "types.h"

#include <span>
#include <vector>

namespace uee {

// SPU ADPCM: a block holds 28 samples as 4-bit nibbles, plus the shift and
// filter they were encoded with.
constexpr usize kAdpcmBlockBytes = 16;
constexpr usize kAdpcmBlockSamples = 28;

// Each block continues the two samples before it, so a stream decoded in pieces
// carries this across.
struct AdpcmState {
    s32 previous = 0;
    s32 older = 0;
};

void decodeAdpcm(std::span<const u8> blocks, AdpcmState& state, std::vector<s16>& out);

}  // namespace uee
