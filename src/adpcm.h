#pragma once

#include "types.h"

#include <span>
#include <vector>

namespace uee {

// SPU ADPCM: 28 samples as 4-bit nibbles, plus their shift and filter.
constexpr usize kAdpcmBlockBytes = 16;
constexpr usize kAdpcmBlockSamples = 28;

// Each block continues the two samples before it.
struct AdpcmState {
    s32 previous = 0;
    s32 older = 0;
};

void decodeAdpcmBlock(std::span<const u8> block, AdpcmState& state, std::span<s16> out);

void decodeAdpcm(std::span<const u8> blocks, AdpcmState& state, std::vector<s16>& out);

}  // namespace uee
