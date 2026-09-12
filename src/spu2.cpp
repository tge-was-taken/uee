// A `<MODULE>: <name> @ <address>` tag attests what a register does:
//   LIBSD   md5 7e79d40b99f8d87a5084188956550cd5
//   MODHSYN md5 cf55a824ff89c067f43f766d6ca78549

#include "spu2.h"

#include "adpcm.h"
#include "log.h"

#include <algorithm>

namespace uee {
namespace {

LoggerChannel log("spu2");

// LIBSD: sceSdSetParam @ 0x39a0, sceSdSetAddr @ 0x3acc, table at 0x4ca0
// A core every 0x400, voice parameters every 0x10, addresses every 0xc,
// volumes every 0x28.
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
constexpr u32 kAutoDma = 0x1B0;

constexpr u32 kVolumeBlock = 0x760;
constexpr u32 kVolumeBlockStride = 0x28;
constexpr u32 kMasterLeft = 0x000;
constexpr u32 kMasterRight = 0x002;

// LIBSD: SpuInitCores @ 0x29ec waits on this bit before it touches a core, and
// freesd waits for it to clear once it has turned transfers off. PCSX2's
// SPU2/spu2sys.cpp REG_C_ATTR moves it with the transfer mode as here.
constexpr u16 kTransferReady = 0x0080;
constexpr u16 kTransferModeMask = 0x0030;

// A volume register in this mode walks towards its target over time.
constexpr u16 kSweepMode = 0x8000;

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

// MODHSYN treats the sustain level as four bits, each step of it 0x800.
constexpr s32 kSustainStepShift = 11;

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
RateStep getRateStep(u32 rate, bool rising, bool exponential, s32 level) {
    const s32 shift = static_cast<s32>(rate >> 2);
    const s32 step = rising ? 7 - static_cast<s32>(rate & 3) : -8 + static_cast<s32>(rate & 3);
    RateStep result;
    // The slowest rates still move, one count per sample.
    result.period = std::max(1, kRatePeriod >> std::max(0, shift - kRateShiftLimit));
    result.value = step << std::max(0, kRateShiftLimit - shift);
    if (exponential && rising && level > kEnvelopeKnee) {
        result.period >>= 2;
    }
    if (exponential && !rising) {
        result.value = static_cast<s32>((static_cast<s64>(result.value) * level) >> 15);
    }
    return result;
}

u32 joinAddress(u16 high, u16 low) {
    return (static_cast<u32>(high & 0x3F) << 16) | low;
}

s16 clampToSample(s32 value) {
    return static_cast<s16>(std::clamp(value, -32768, 32767));
}

}  // namespace

Spu2::Spu2() : mMemory(kMemoryHalves, 0) {}

Spu2::Voice* Spu2::findVoice(u32 core, u32 offset, u32 base, u32 stride) {
    if (offset < base) {
        return nullptr;
    }
    const u32 index = (offset - base) / stride;
    return index < kVoices ? &mCores[core].voices[index] : nullptr;
}

const Spu2::Voice* Spu2::findVoice(u32 core, u32 offset, u32 base, u32 stride) const {
    return const_cast<Spu2*>(this)->findVoice(core, offset, base, stride);
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
        const Voice* voice = findVoice(core, local, 0, kVoiceStride);
        switch (local % kVoiceStride) {
            case kVolumeLeft: return voice->volumeLeft;
            case kVolumeRight: return voice->volumeRight;
            case kPitch: return voice->pitch;
            case kAdsr1: return voice->adsr1;
            case kAdsr2: return voice->adsr2;
            // The envelope's current value is readable while it runs.
            case kEnvelopeValue: return static_cast<u16>(voice->envelope.level);
            default: return 0;
        }
    }

    if (local >= kVoiceAddresses && local < kEndAddresses) {
        const Voice* voice = findVoice(core, local, kVoiceAddresses, kAddressStride);
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
        case kStatus: return mCores[core].status;
        case kAutoDma: return mCores[core].autoDma;
        default: return 0;
    }
}

// Transfers complete as they are started here, so none is ever in flight.
void Spu2::writeAttribute(Core& core, u16 value) {
    const bool wasTransferring = (core.attribute & kTransferModeMask) != 0;
    const bool transferring = (value & kTransferModeMask) != 0;
    if (!transferring) {
        core.status &= ~kTransferReady;
    } else if (!wasTransferring) {
        core.status |= kTransferReady;
    }
    core.attribute = value;
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
        Voice* voice = findVoice(core, local, 0, kVoiceStride);
        switch (local % kVoiceStride) {
            case kVolumeLeft: voice->volumeLeft = value; return true;
            case kVolumeRight: voice->volumeRight = value; return true;
            case kPitch: voice->pitch = value; return true;
            case kAdsr1: voice->adsr1 = value; return true;
            case kAdsr2: voice->adsr2 = value; return true;
            case kEnvelopeValue: voice->envelope.level = value; return true;
            default: return true;
        }
    }

    if (local >= kVoiceAddresses && local < kEndAddresses) {
        Voice* voice = findVoice(core, local, kVoiceAddresses, kAddressStride);
        const u32 field = (local - kVoiceAddresses) % kAddressStride;
        u32& address = field < 4 ? voice->startAddress
                                 : (field < 8 ? voice->loopAddress : voice->nextAddress);
        address = (field % 4) == 0 ? joinAddress(value, static_cast<u16>(address))
                                   : joinAddress(static_cast<u16>(address >> 16), value);
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
        case kAttribute: writeAttribute(mCores[core], value); return true;
        case kAutoDma: mCores[core].autoDma = value; return true;
        case kKeyOn: keyOn(mCores[core], value); return true;
        case kKeyOn + 2: keyOn(mCores[core], static_cast<u32>(value) << 16); return true;
        case kKeyOff: keyOff(mCores[core], value); return true;
        case kKeyOff + 2: keyOff(mCores[core], static_cast<u32>(value) << 16); return true;
        case kTransferAddress:
            mCores[core].transferAddress = joinAddress(value, static_cast<u16>(mCores[core].transferAddress));
            return true;
        case kTransferAddress + 2:
            mCores[core].transferAddress =
                joinAddress(static_cast<u16>(mCores[core].transferAddress >> 16), value);
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
        if ((voices & (1u << i)) != 0 && core.voices[i].envelope.phase != EnvelopePhase::Off) {
            core.voices[i].envelope.phase = EnvelopePhase::Release;
            core.voices[i].envelope.counter = 0;
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
    voice.envelope = {EnvelopePhase::Attack, 0, 0};
    voice.left = {};
    voice.right = {};
    voice.loopSet = false;
    voice.ended = false;
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

// MODHSYN: HSynVoiceVolumeCommit @ 0x4c80
// Fifteen bits, negated for the opposite phase. No module here walks one.
s32 Spu2::applyVolume(s32 sample, u16 volume, Sweep& sweep) {
    if ((volume & kSweepMode) == 0) {
        sweep.level = static_cast<s16>(volume << 1);
        return (sample * sweep.level) >> 15;
    }
    const bool rising = (volume & 0x2000) == 0;
    const bool exponential = (volume & 0x4000) != 0;
    const RateStep step = getRateStep(volume & 0x7F, rising, exponential, std::abs(sweep.level));
    sweep.counter += step.period;
    while (sweep.counter >= kRatePeriod) {
        sweep.counter -= kRatePeriod;
        sweep.level = std::clamp(sweep.level + step.value, -kEnvelopeFull, kEnvelopeFull);
    }
    return (sample * sweep.level) >> 15;
}

// MODHSYN: HSynBuildAdsr1 @ 0x40ac, HSynBuildAdsr2 @ 0x4150, HSynLfoApply @ 0x7834
// Adsr1 patches 7 bits at 8, 4 at 4, 4 at 0; Adsr2 7 at 6 and 5 at 0. A
// release writes 0xc021: curve in the top three bits, release curve at bit 5.
void stepEnvelope(Envelope& envelope, u16 adsr1, u16 adsr2) {
    const u32 attackRate = (adsr1 >> 8) & 0x7F;
    const bool attackExponential = (adsr1 & 0x8000) != 0;
    const u32 decayRate = (adsr1 >> 4) & 0x0F;
    const s32 sustainLevel = static_cast<s32>((adsr1 & 0x0F) + 1) << kSustainStepShift;
    const u32 sustainRate = (adsr2 >> 6) & 0x7F;
    const u32 sustainMode = (adsr2 >> 13) & 0x07;
    const u32 releaseRate = adsr2 & 0x1F;
    const bool releaseExponential = (adsr2 & 0x20) != 0;

    RateStep step;
    switch (envelope.phase) {
        case EnvelopePhase::Attack:
            step = getRateStep(attackRate, true, attackExponential, envelope.level);
            break;
        case EnvelopePhase::Decay:
            step = getRateStep(decayRate << 2, false, true, envelope.level);
            break;
        case EnvelopePhase::Sustain:
            step = getRateStep(sustainRate, (sustainMode & 2) == 0, (sustainMode & 4) != 0,
                          envelope.level);
            break;
        case EnvelopePhase::Release:
            step = getRateStep(releaseRate << 2, false, releaseExponential, envelope.level);
            break;
        case EnvelopePhase::Off: return;
    }

    envelope.counter += step.period;
    if (envelope.counter >= kRatePeriod) {
        envelope.counter = 0;
        envelope.level = std::clamp(envelope.level + step.value, 0, kEnvelopeFull);
    }

    // A sustain that falls to nothing ends the voice the way a release does.
    if (envelope.phase == EnvelopePhase::Attack && envelope.level >= kEnvelopeFull) {
        envelope.phase = EnvelopePhase::Decay;
    } else if (envelope.phase == EnvelopePhase::Decay && envelope.level <= sustainLevel) {
        envelope.phase = EnvelopePhase::Sustain;
    } else if ((envelope.phase == EnvelopePhase::Release ||
                envelope.phase == EnvelopePhase::Sustain) &&
               envelope.level == 0) {
        envelope.phase = EnvelopePhase::Off;
    }
}

s32 Spu2::stepVoice(Core& core, Voice& voice, u32 index) {
    if (voice.envelope.phase == EnvelopePhase::Off) {
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
                    voice.envelope = {};
                    return 0;
                }
            }
            decodeBlock(voice);
        }
    }

    const s32 sample = voice.history + (((voice.decoded[voice.sampleIndex] - voice.history) *
                                         static_cast<s32>(voice.counter)) >>
                                        12);
    stepEnvelope(voice.envelope, voice.adsr1, voice.adsr2);
    return (sample * voice.envelope.level) >> 15;
}

// Off and at a fixed volume, a voice adds nothing and nothing about it moves.
bool Spu2::isSilent(const Voice& voice) {
    return voice.envelope.phase == EnvelopePhase::Off && (voice.volumeLeft & kSweepMode) == 0 &&
           (voice.volumeRight & kSweepMode) == 0;
}

// Nothing sounds and no volume walks, so no frame changes anything.
bool Spu2::isIdle() const {
    for (const Core& core : mCores) {
        if ((core.masterLeft & kSweepMode) != 0 || (core.masterRight & kSweepMode) != 0) {
            return false;
        }
        for (const Voice& voice : core.voices) {
            if (!isSilent(voice)) {
                return false;
            }
        }
    }
    return true;
}

// What one idle frame leaves a silent voice, which is also what any number do.
void Spu2::syncLevels(Voice& voice) {
    voice.left.level = static_cast<s16>(voice.volumeLeft << 1);
    voice.right.level = static_cast<s16>(voice.volumeRight << 1);
}

void Spu2::syncLevels() {
    for (Core& core : mCores) {
        core.left.level = static_cast<s16>(core.masterLeft << 1);
        core.right.level = static_cast<s16>(core.masterRight << 1);
        for (Voice& voice : core.voices) {
            syncLevels(voice);
        }
    }
}

void Spu2::advance(u64 frames) {
    constexpr u64 kChunkFrames = 256;
    if (isIdle()) {
        syncLevels();
        return;
    }
    std::vector<s16> stereo(kChunkFrames * 2);
    for (u64 done = 0; done < frames; done += kChunkFrames) {
        const usize count = static_cast<usize>(std::min(kChunkFrames, frames - done));
        render(std::span(stereo).first(count * 2));
    }
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
                if (isSilent(voice)) {
                    syncLevels(voice);
                    continue;
                }
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
