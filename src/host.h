#pragma once

#include "types.h"

#include <span>

namespace uee {

// What the sound processor plays at, so anything writing these samples out says
// the rate and width the guest produced them at.
constexpr u32 kSpuSampleRate = 48'000;
constexpr u16 kSpuChannels = 2;

class AudioSink {
public:
    virtual ~AudioSink() = default;

    virtual void write(std::span<const s16> samples) = 0;
};

class PadSource {
public:
    virtual ~PadSource() = default;

    // 0 = active so nothing pressed is all ones.
    virtual u16 buttons() = 0;
};

}  // namespace uee
