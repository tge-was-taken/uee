#include "iop_mmio.h"

#include "iop_kernel.h"
#include "timers.h"
#include "log.h"

#include <vector>

namespace uee {
namespace {

LoggerChannel log("iopmmio");

constexpr u32 kChannelSpan = 0x10;
constexpr u32 kMadr = 0x0;
constexpr u32 kBcr = 0x4;
constexpr u32 kChcr = 0x8;

// LIBSD: SpuBlockTransStart @ 0x2f4c writes this to start a transfer to SPU2.
constexpr u32 kStart = 0x01000000;

// SPU2 plays at 48 kHz, so a transfer takes as long as its samples do.
constexpr u64 kIopCyclesPerSample = kIopClockHz / kSpuSampleRate;

constexpr u64 samplesIn(u32 words) {
    return static_cast<u64>(words) * 4 / kAdpcmBlockBytes * kAdpcmBlockSamples;
}

// SIO2MAN md5 ea22447abe3a4b56a26f3149316370a3 drives the pads and cards here.
constexpr u32 kSio2Base = 0x1F808200;
constexpr u32 kSio2Size = 0x100;

constexpr u32 kCoreStride = 0x400;
constexpr u32 kCore0Statx = kSpu2Base + 0x344;
constexpr u32 kCore1Statx = kCore0Statx + kCoreStride;
constexpr u16 kTransferReady = 0x0080;

}  // namespace

void IopMmio::attach(Memory& memory, IopKernel& kernel) {
    mMemory = &memory;
    mKernel = &kernel;
    mSio2.attach(kernel);
    memory.attach(this);
}

IopMmio::SpuChannel* IopMmio::channelFor(u32 address) {
    for (SpuChannel& channel : mChannels) {
        if (address - channel.base < kChannelSpan) {
            return &channel;
        }
    }
    return nullptr;
}

const IopMmio::SpuChannel* IopMmio::channelFor(u32 address) const {
    return const_cast<IopMmio*>(this)->channelFor(address);
}

// LIBSD: SpuInitCores @ 0x29ec waits on bit 7 of STATX before it touches a core
std::optional<u16> IopMmio::readHalf(u32 address) const {
    if (address == kCore0Statx || address == kCore1Statx) {
        return kTransferReady;
    }
    const SpuChannel* channel = channelFor(address);
    if (channel == nullptr) {
        return std::nullopt;
    }
    const std::optional<u32> word = readWord(address & ~3u);
    if (!word) {
        return std::nullopt;
    }
    return static_cast<u16>((address & 2) != 0 ? *word >> 16 : *word);
}

// LIBSD programs the channel a halfword at a time, so the word the start bit
// arrives in has to be rebuilt from the half already there.
bool IopMmio::writeHalf(u32 address, u16 value) {
    SpuChannel* channel = channelFor(address);
    if (channel == nullptr) {
        return false;
    }
    const u32 word = address & ~3u;
    const std::optional<u32> held = readWord(word);
    if (!held) {
        return false;
    }
    const u32 merged = (address & 2) != 0 ? (*held & 0xFFFFu) | (static_cast<u32>(value) << 16)
                                          : (*held & 0xFFFF0000u) | value;
    return writeWord(word, merged);
}

std::optional<u32> IopMmio::readWord(u32 address) const {
    if (address - kSio2Base < kSio2Size) {
        return const_cast<Sio2&>(mSio2).read(address - kSio2Base);
    }
    const SpuChannel* channel = channelFor(address);
    if (channel == nullptr) {
        return std::nullopt;
    }
    switch (address - channel->base) {
        case kMadr: return channel->madr;
        case kBcr: return channel->bcr;
        case kChcr: return channel->chcr;
        default: return std::nullopt;
    }
}

void IopMmio::sendToAudio(const SpuChannel& channel, u32 words) {
    if (mAudio == nullptr || words == 0) {
        return;
    }
    std::vector<u8> blocks(static_cast<usize>(words) * 4);
    for (usize i = 0; i < blocks.size(); ++i) {
        blocks[i] = mMemory->read8(channel.madr + static_cast<u32>(i));
    }
    std::vector<s16> samples;
    decodeAdpcm(blocks, mAdpcm, samples);
    mAudio->write(samples);
}

bool IopMmio::writeWord(u32 address, u32 value) {
    if (address - kSio2Base < kSio2Size) {
        return mSio2.write(address - kSio2Base, value);
    }
    SpuChannel* channel = channelFor(address);
    if (channel == nullptr) {
        return false;
    }
    switch (address - channel->base) {
        case kMadr: channel->madr = value; return true;
        case kBcr: channel->bcr = value; return true;
        case kChcr: break;
        default: return false;
    }

    // Normally this would start transfer to sound RAM, we just signal for completion. 
    // LIBSD waits on the interrupt, not on the data.
    channel->chcr = value & ~kStart;
    if ((value & kStart) != 0) {
        const u32 words = (channel->bcr & 0xFFFF) * (channel->bcr >> 16);
        log.trace("spu dma from {:08x}, {} words, line {}", channel->madr, words, channel->line);
        sendToAudio(*channel, words);
        mKernel->raiseInterruptAfter(channel->line, kIopCyclesPerSample * samplesIn(words));
    }
    return true;
}

}  // namespace uee
