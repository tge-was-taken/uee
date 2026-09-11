#pragma once

#include "types.h"

#include <optional>
#include <span>
#include <vector>

namespace uee {

// The volume a voice plays at, walked once per output sample through the four
// phases its two ADSR registers describe.
enum class EnvelopePhase { Off, Attack, Decay, Sustain, Release };

struct Envelope {
    EnvelopePhase phase = EnvelopePhase::Off;
    s32 level = 0;
    s32 counter = 0;
};

void stepEnvelope(Envelope& envelope, u16 adsr1, u16 adsr2);

// The sound processor: two cores of 24 voices, reading ADPCM out of two
// megabytes of local memory and mixing what they play into one stereo pair.
class Spu2 {
public:
    Spu2();

    std::optional<u16> readHalf(u32 offset) const;
    bool writeHalf(u32 offset, u16 value);

    // A DMA to a core, landing at the address its transfer register names.
    void receive(u32 core, std::span<const u8> bytes);

    // Interleaved left then right.
    void render(std::span<s16> stereo);

private:
    static constexpr usize kCores = 2;
    static constexpr usize kVoices = 24;
    static constexpr usize kMemoryHalves = 1024 * 1024;

    // A rate can walk a volume over time, on the counter the envelope keeps.
    struct Sweep {
        s32 level = 0;
        s32 counter = 0;
    };

    struct Voice {
        u16 volumeLeft = 0;
        u16 volumeRight = 0;
        u16 pitch = 0;
        u16 adsr1 = 0;
        u16 adsr2 = 0;
        u32 startAddress = 0;
        u32 loopAddress = 0;
        u32 nextAddress = 0;

        // A block holds 28 samples, and reading one is what advances the
        // address the NAXH/L register reports.
        s16 decoded[28] = {};
        u32 blockAddress = 0;
        u32 sampleIndex = 0;
        u32 counter = 0;
        s32 previous = 0;
        s32 older = 0;
        s32 history = 0;

        Envelope envelope;
        Sweep left;
        Sweep right;
        bool loopSet = false;
        bool ended = false;
        bool repeats = false;
    };

    struct Core {
        Voice voices[kVoices];
        u32 mixLeft = 0;
        u32 mixRight = 0;
        u32 endx = 0;
        u32 transferAddress = 0;
        u16 attribute = 0;
        u16 masterLeft = 0;
        u16 masterRight = 0;
        Sweep left;
        Sweep right;
    };

    Voice* findVoice(u32 core, u32 offset, u32 base, u32 stride);
    const Voice* findVoice(u32 core, u32 offset, u32 base, u32 stride) const;

    void keyOn(Core& core, u32 voices);
    void keyOff(Core& core, u32 voices);
    void startVoice(Voice& voice);
    void decodeBlock(Voice& voice);
    s32 stepVoice(Core& core, Voice& voice, u32 index);
    s32 applyVolume(s32 sample, u16 volume, Sweep& sweep);

    std::vector<u16> mMemory;
    Core mCores[kCores];
};

}  // namespace uee
