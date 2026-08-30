// A `<MODULE>: <name> @ <address>` tag names the routine that attests what the
// hardware does with a register, in the modules the game loads:
//   LIBSD   md5 7e79d40b99f8d87a5084188956550cd5
//   MODHSYN md5 cf55a824ff89c067f43f766d6ca78549

#include "spu2.h"

#include "adpcm.h"
#include "log.h"

#include <algorithm>

namespace uee {
namespace {

LoggerChannel log("spu2");

// LIBSD keeps the register addresses in a table at 0x4ca0 and reaches them with
// sceSdSetParam @ 0x39a0 and sceSdSetAddr @ 0x3acc: a core every 0x400, a
// voice's parameters every 0x10 and its addresses every 0xc, with the volumes
// in a block of their own every 0x28.
constexpr u32 kCoreStride = 0x400;
constexpr u32 kVoiceStride = 0x10;
constexpr u32 kAddressStride = 0xC;

constexpr u32 kVolumeLeft = 0x000;
constexpr u32 kVolumeRight = 0x002;
constexpr u32 kPitch = 0x004;
constexpr u32 kAdsr1 = 0x006;
constexpr u32 kAdsr2 = 0x008;
constexpr u32 kEnvelopeValue = 0x00A;
constexpr u32 kVoiceRegisters = 0x180;

constexpr u32 kMixLeft = 0x188;
constexpr u32 kMixRight = 0x190;
constexpr u32 kAttribute = 0x19A;
constexpr u32 kKeyOn = 0x1A0;
constexpr u32 kKeyOff = 0x1A4;
constexpr u32 kTransferAddress = 0x1A8;
constexpr u32 kStartAddress = 0x1C0;
constexpr u32 kLoopAddress = 0x1C4;
constexpr u32 kNextAddress = 0x1C8;
constexpr u32 kVoiceAddresses = 0x1C0;
constexpr u32 kEndAddresses = 0x1C0 + 24 * kAddressStride;
constexpr u32 kEndx = 0x340;
constexpr u32 kStatus = 0x344;

constexpr u32 kVolumeBlock = 0x760;
constexpr u32 kVolumeBlockStride = 0x28;
constexpr u32 kMasterLeft = 0x000;
constexpr u32 kMasterRight = 0x002;

// LIBSD: SpuInitCores @ 0x29ec waits on this bit before it touches a core.
constexpr u16 kTransferReady = 0x0080;

// LIBSD: SpuResetVoices @ 0x4344 points every voice at a block of 00 07 and
// silence, so a block that plays nothing for ever has all three of these set.
constexpr u16 kLoopStart = 1u << 10;
constexpr u16 kLoopRepeat = 1u << 9;
constexpr u16 kLoopEnd = 1u << 8;

// The envelope runs to this. The point at which a pseudo exponential rise slows
// down is not attested by anything cited here.
constexpr s32 kEnvelopeFull = 0x7FFF;
constexpr s32 kEnvelopeKnee = 0x6000;

// The pitch reaches two octaves above the original sound, and 0x1000 plays it
// back unchanged.
constexpr u32 kPitchUnity = 0x1000;
constexpr u32 kPitchLimit = 0x3FFF;

// MODHSYN treats the sustain level as four bits. The level each of its sixteen
// values stands for is not attested by anything cited here.
constexpr s32 kSustainSteps = 16;

// A rate counts down from this, so a rate whose period is longer than one
// sample only steps every few of them.
constexpr s32 kRatePeriod = 0x8000;
constexpr s32 kRateShiftLimit = 11;

struct RateStep {
    s32 period = 0;
    s32 value = 0;
};

// A rate's low two bits pick how much is added and the rest how often, so one
// step up takes twice as long to cross the same range.
RateStep stepOf(u32 rate, bool rising, bool exponential, s32 level) {
    const s32 shift = static_cast<s32>(rate >> 2);
    const s32 step = rising ? 7 - static_cast<s32>(rate & 3) : -8 + static_cast<s32>(rate & 3);
    RateStep result;
    result.period = kRatePeriod >> std::max(0, shift - kRateShiftLimit);
    result.value = step << std::max(0, kRateShiftLimit - shift);
    if (exponential && rising && level > kEnvelopeKnee) {
        result.period >>= 2;
    }
    if (exponential && !rising) {
        result.value = static_cast<s32>((static_cast<s64>(result.value) * level) >> 15);
    }
    return result;
}

u32 addressOf(u16 high, u16 low) {
    return (static_cast<u32>(high & 0x3F) << 16) | low;
}

s16 clampToSample(s32 value) {
    return static_cast<s16>(std::clamp(value, -32768, 32767));
}

}  // namespace

Spu2::Spu2() : mMemory(kMemoryHalves, 0) {}

Spu2::Voice* Spu2::voiceAt(u32 core, u32 offset, u32 base, u32 stride) {
    if (offset < base) {
        return nullptr;
    }
    const u32 index = (offset - base) / stride;
    return index < kVoices ? &mCores[core].voices[index] : nullptr;
}

const Spu2::Voice* Spu2::voiceAt(u32 core, u32 offset, u32 base, u32 stride) const {
    return const_cast<Spu2*>(this)->voiceAt(core, offset, base, stride);
}

std::optional<u16> Spu2::readHalf(u32 offset) const {
    if (offset >= kVolumeBlock) {
        const u32 core = (offset - kVolumeBlock) / kVolumeBlockStride;
        if (core >= kCores) {
            return std::nullopt;
        }
        switch ((offset - kVolumeBlock) % kVolumeBlockStride) {
            case kMasterLeft: return mCores[core].masterLeft;
            case kMasterRight: return mCores[core].masterRight;
            default: return std::nullopt;
        }
    }

    const u32 core = offset / kCoreStride;
    const u32 local = offset % kCoreStride;
    if (core >= kCores) {
        return std::nullopt;
    }

    if (local < kVoiceRegisters) {
        const Voice* voice = voiceAt(core, local, 0, kVoiceStride);
        switch (local % kVoiceStride) {
            case kVolumeLeft: return voice->volumeLeft;
            case kVolumeRight: return voice->volumeRight;
            case kPitch: return voice->pitch;
            case kAdsr1: return voice->adsr1;
            case kAdsr2: return voice->adsr2;
            // The envelope's current value is readable while it runs.
            case kEnvelopeValue: return static_cast<u16>(voice->envelope);
            default: return 0;
        }
    }

    if (local >= kVoiceAddresses && local < kEndAddresses) {
        const Voice* voice = voiceAt(core, local, kVoiceAddresses, kAddressStride);
        const u32 field = (local - kVoiceAddresses) % kAddressStride;
        // The address a voice is playing from is what the host polls.
        const u32 address = field < 4 ? voice->startAddress
                                      : (field < 8 ? voice->loopAddress : voice->nextAddress);
        return static_cast<u16>((field % 4) == 0 ? address >> 16 : address);
    }

    switch (local) {
        case kAttribute: return mCores[core].attribute;
        case kEndx: return static_cast<u16>(mCores[core].endx);
        case kEndx + 2: return static_cast<u16>(mCores[core].endx >> 16);
        case kStatus: return kTransferReady;
        default: return 0;
    }
}

bool Spu2::writeHalf(u32 offset, u16 value) {
    if (offset >= kVolumeBlock) {
        const u32 core = (offset - kVolumeBlock) / kVolumeBlockStride;
        if (core >= kCores) {
            return false;
        }
        switch ((offset - kVolumeBlock) % kVolumeBlockStride) {
            case kMasterLeft: mCores[core].masterLeft = value; return true;
            case kMasterRight: mCores[core].masterRight = value; return true;
            default: return true;
        }
    }

    const u32 core = offset / kCoreStride;
    const u32 local = offset % kCoreStride;
    if (core >= kCores) {
        return false;
    }

    if (local < kVoiceRegisters) {
        Voice* voice = voiceAt(core, local, 0, kVoiceStride);
        switch (local % kVoiceStride) {
            case kVolumeLeft: voice->volumeLeft = value; return true;
            case kVolumeRight: voice->volumeRight = value; return true;
            case kPitch: voice->pitch = value; return true;
            case kAdsr1: voice->adsr1 = value; return true;
            case kAdsr2: voice->adsr2 = value; return true;
            case kEnvelopeValue: voice->envelope = value; return true;
            default: return true;
        }
    }

    if (local >= kVoiceAddresses && local < kEndAddresses) {
        Voice* voice = voiceAt(core, local, kVoiceAddresses, kAddressStride);
        const u32 field = (local - kVoiceAddresses) % kAddressStride;
        u32& address = field < 4 ? voice->startAddress
                                 : (field < 8 ? voice->loopAddress : voice->nextAddress);
        address = (field % 4) == 0 ? addressOf(value, static_cast<u16>(address))
                                   : addressOf(static_cast<u16>(address >> 16), value);
        // A loop address written by the host outlasts the flag in the waveform
        // data until the voice is keyed on again.
        if (field >= 4 && field < 8) {
            voice->loopSet = true;
        }
        return true;
    }

    switch (local) {
        case kMixLeft: mCores[core].mixLeft = (mCores[core].mixLeft & 0xFFFF0000) | value; return true;
        case kMixLeft + 2: mCores[core].mixLeft = (mCores[core].mixLeft & 0xFFFF) | (value << 16); return true;
        case kMixRight: mCores[core].mixRight = (mCores[core].mixRight & 0xFFFF0000) | value; return true;
        case kMixRight + 2: mCores[core].mixRight = (mCores[core].mixRight & 0xFFFF) | (value << 16); return true;
        case kAttribute: mCores[core].attribute = value; return true;
        case kKeyOn: keyOn(mCores[core], value); return true;
        case kKeyOn + 2: keyOn(mCores[core], static_cast<u32>(value) << 16); return true;
        case kKeyOff: keyOff(mCores[core], value); return true;
        case kKeyOff + 2: keyOff(mCores[core], static_cast<u32>(value) << 16); return true;
        case kTransferAddress:
            mCores[core].transferAddress = addressOf(value, static_cast<u16>(mCores[core].transferAddress));
            return true;
        case kTransferAddress + 2:
            mCores[core].transferAddress =
                addressOf(static_cast<u16>(mCores[core].transferAddress >> 16), value);
            return true;
        // A written one clears the bit of a voice that has ended.
        case kEndx: mCores[core].endx &= ~static_cast<u32>(value); return true;
        case kEndx + 2: mCores[core].endx &= ~(static_cast<u32>(value) << 16); return true;
        default: return true;
    }
}

void Spu2::receive(u32 core, std::span<const u8> bytes) {
    if (core >= kCores) {
        return;
    }
    u32 at = mCores[core].transferAddress;
    for (usize i = 0; i + 1 < bytes.size(); i += 2) {
        if (at >= mMemory.size()) {
            at = 0;
        }
        mMemory[at++] = static_cast<u16>(bytes[i] | (bytes[i + 1] << 8));
    }
    mCores[core].transferAddress = at;
}

// A voice starts from the address in its SSAH/L register, and the loop address
// it had is forgotten.
void Spu2::keyOn(Core& core, u32 voices) {
    log.trace("key on {:06x}", voices);
    for (u32 i = 0; i < kVoices; ++i) {
        if ((voices & (1u << i)) != 0) {
            startVoice(core.voices[i]);
            core.endx &= ~(1u << i);
        }
    }
}

void Spu2::keyOff(Core& core, u32 voices) {
    log.trace("key off {:06x}", voices);
    for (u32 i = 0; i < kVoices; ++i) {
        if ((voices & (1u << i)) != 0 && core.voices[i].phase != Phase::Off) {
            core.voices[i].phase = Phase::Release;
        }
    }
}

void Spu2::startVoice(Voice& voice) {
    voice.blockAddress = voice.startAddress;
    voice.nextAddress = voice.startAddress;
    voice.sampleIndex = 0;
    voice.counter = 0;
    voice.previous = 0;
    voice.older = 0;
    voice.history = 0;
    voice.envelope = 0;
    voice.envelopeCounter = 0;
    voice.left = {};
    voice.right = {};
    voice.loopSet = false;
    voice.ended = false;
    voice.phase = Phase::Attack;
    decodeBlock(voice);
}

void Spu2::decodeBlock(Voice& voice) {
    if (voice.blockAddress + kAdpcmBlockBytes / 2 > mMemory.size()) {
        voice.blockAddress = 0;
    }
    u8 block[kAdpcmBlockBytes];
    for (usize i = 0; i < kAdpcmBlockBytes / 2; ++i) {
        const u16 half = mMemory[voice.blockAddress + i];
        block[i * 2] = static_cast<u8>(half);
        block[i * 2 + 1] = static_cast<u8>(half >> 8);
    }

    const u16 header = static_cast<u16>(block[0] | (block[1] << 8));
    if ((header & kLoopStart) != 0 && !voice.loopSet) {
        voice.loopAddress = voice.blockAddress;
    }
    voice.ended = (header & kLoopEnd) != 0;
    voice.repeats = (header & kLoopRepeat) != 0;
    voice.nextAddress = voice.blockAddress;

    AdpcmState state{voice.previous, voice.older};
    decodeAdpcmBlock(block, state, voice.decoded);
    voice.previous = state.previous;
    voice.older = state.older;

    // The block after an endpoint is the loop point, or nothing at all.
    voice.blockAddress = voice.ended && voice.repeats
                             ? voice.loopAddress
                             : voice.blockAddress + kAdpcmBlockBytes / 2;
}

// MODHSYN: HSynVoiceVolumeCommit @ 0x4c80 writes a volume as a fifteen bit
// value it negates for the opposite phase. The modes that walk one over time
// follow the envelope's rates; no module here uses one.
s32 Spu2::applyVolume(s32 sample, u16 volume, Sweep& sweep) {
    if ((volume & 0x8000) == 0) {
        sweep.level = static_cast<s16>(volume << 1);
        return (sample * sweep.level) >> 15;
    }
    const bool rising = (volume & 0x2000) == 0;
    const bool exponential = (volume & 0x4000) != 0;
    const RateStep step = stepOf(volume & 0x7F, rising, exponential, std::abs(sweep.level));
    sweep.counter += step.period;
    while (sweep.counter >= kRatePeriod) {
        sweep.counter -= kRatePeriod;
        sweep.level = std::clamp(sweep.level + step.value, -kEnvelopeFull, kEnvelopeFull);
    }
    return (sample * sweep.level) >> 15;
}

// MODHSYN: HSynBuildAdsr1 @ 0x40ac patches seven bits at bit 8, four at bit 4
// and four at bit 0, HSynBuildAdsr2 @ 0x4150 seven at bit 6 and five at bit 0,
// and HSynLfoApply @ 0x7834 writes 0xc021 to release a voice, which puts the
// curve in the top three bits and the release curve at bit 5.
void Spu2::stepEnvelope(Voice& voice) {
    if (voice.phase == Phase::Off) {
        return;
    }
    const u32 attackRate = (voice.adsr1 >> 8) & 0x7F;
    const bool attackExponential = (voice.adsr1 & 0x8000) != 0;
    const u32 decayRate = (voice.adsr1 >> 4) & 0x0F;
    const s32 sustainLevel = static_cast<s32>((voice.adsr1 & 0x0F) + 1) * kEnvelopeFull /
                             kSustainSteps;
    const u32 sustainRate = (voice.adsr2 >> 6) & 0x7F;
    const u32 sustainMode = (voice.adsr2 >> 13) & 0x07;
    const u32 releaseRate = voice.adsr2 & 0x1F;
    const bool releaseExponential = (voice.adsr2 & 0x20) != 0;

    RateStep step;
    switch (voice.phase) {
        case Phase::Attack: step = stepOf(attackRate, true, attackExponential, voice.envelope); break;
        case Phase::Decay: step = stepOf(decayRate << 2, false, true, voice.envelope); break;
        case Phase::Sustain:
            step = stepOf(sustainRate, (sustainMode & 2) == 0, (sustainMode & 4) != 0,
                          voice.envelope);
            break;
        case Phase::Release:
            step = stepOf(releaseRate << 2, false, releaseExponential, voice.envelope);
            break;
        case Phase::Off: return;
    }

    voice.envelopeCounter += step.period;
    while (voice.envelopeCounter >= kRatePeriod) {
        voice.envelopeCounter -= kRatePeriod;
        voice.envelope = std::clamp(voice.envelope + step.value, 0, kEnvelopeFull);
    }

    if (voice.phase == Phase::Attack && voice.envelope >= kEnvelopeFull) {
        voice.phase = Phase::Decay;
    } else if (voice.phase == Phase::Decay && voice.envelope <= sustainLevel) {
        voice.phase = Phase::Sustain;
    } else if (voice.phase == Phase::Release && voice.envelope == 0) {
        voice.phase = Phase::Off;
    }
}

s32 Spu2::stepVoice(Core& core, Voice& voice, u32 index) {
    if (voice.phase == Phase::Off) {
        return 0;
    }

    voice.counter += std::min<u32>(voice.pitch, kPitchLimit);
    while (voice.counter >= kPitchUnity) {
        voice.counter -= kPitchUnity;
        voice.history = voice.decoded[voice.sampleIndex];
        ++voice.sampleIndex;
        if (voice.sampleIndex == kAdpcmBlockSamples) {
            voice.sampleIndex = 0;
            if (voice.ended) {
                core.endx |= 1u << index;
                // Waveform data with no loop stops the voice and leaves the
                // envelope reading zero.
                if (!voice.repeats) {
                    voice.phase = Phase::Off;
                    voice.envelope = 0;
                    return 0;
                }
            }
            decodeBlock(voice);
        }
    }

    const s32 sample = voice.history + (((voice.decoded[voice.sampleIndex] - voice.history) *
                                         static_cast<s32>(voice.counter)) >>
                                        12);
    stepEnvelope(voice);
    return (sample * voice.envelope) >> 15;
}

// A voice reaches a channel only through its own volume and the core's mixing
// switch, and the core's master volume scales what comes out.
void Spu2::render(std::span<s16> stereo) {
    for (usize frame = 0; frame + 1 < stereo.size(); frame += 2) {
        s32 left = 0;
        s32 right = 0;
        for (Core& core : mCores) {
            s32 coreLeft = 0;
            s32 coreRight = 0;
            for (u32 i = 0; i < kVoices; ++i) {
                Voice& voice = core.voices[i];
                // A volume walks whether or not the switch lets the voice
                // through, so both are applied and only the sum is gated.
                const s32 sample = stepVoice(core, voice, i);
                const s32 onLeft = applyVolume(sample, voice.volumeLeft, voice.left);
                const s32 onRight = applyVolume(sample, voice.volumeRight, voice.right);
                if ((core.mixLeft & (1u << i)) != 0) {
                    coreLeft += onLeft;
                }
                if ((core.mixRight & (1u << i)) != 0) {
                    coreRight += onRight;
                }
            }
            left += applyVolume(coreLeft, core.masterLeft, core.left);
            right += applyVolume(coreRight, core.masterRight, core.right);
        }
        stereo[frame] = clampToSample(left);
        stereo[frame + 1] = clampToSample(right);
    }
}

}  // namespace uee
