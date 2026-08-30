#include "adpcm.h"

#include <algorithm>

namespace uee {
namespace {

// The filter weights the hardware applies to the two previous samples.
constexpr s32 kPrevious[5] = {0, 60, 115, 98, 122};
constexpr s32 kOlder[5] = {0, 0, -52, -55, -60};

constexpr u32 kWidestShift = 12;
constexpr u32 kShiftForWider = 9;

s16 clampToSample(s32 value) {
    return static_cast<s16>(std::clamp(value, -32768, 32767));
}

}  // namespace

void decodeAdpcm(std::span<const u8> blocks, AdpcmState& state, std::vector<s16>& out) {
    for (usize block = 0; block + kAdpcmBlockBytes <= blocks.size(); block += kAdpcmBlockBytes) {
        const u8 header = blocks[block];
        const u32 shift = (header & 0x0Fu) > kWidestShift ? kShiftForWider : (header & 0x0Fu);
        const usize filter = std::min<usize>((header >> 4) & 0x07u, 4);

        for (usize i = 0; i < kAdpcmBlockSamples; ++i) {
            const u8 byte = blocks[block + 2 + i / 2];
            const u32 nibble = (i % 2 == 0) ? (byte & 0x0Fu) : (byte >> 4);
            s32 sample = static_cast<s16>(nibble << 12) >> shift;
            sample += (state.previous * kPrevious[filter] + state.older * kOlder[filter]) >> 6;
            const s16 decoded = clampToSample(sample);
            out.push_back(decoded);
            state.older = state.previous;
            state.previous = decoded;
        }
    }
}

}  // namespace uee
