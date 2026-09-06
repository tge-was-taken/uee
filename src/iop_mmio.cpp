#include "iop_mmio.h"

#include "adpcm.h"
#include "iop_kernel.h"
#include "timers.h"
#include "log.h"

#include <algorithm>
#include <format>
#include <stdexcept>
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

// How many frames are rendered at once, so a slice of guest time costs one call
// into the sink rather than one per sample.
constexpr usize kRenderFrames = 256;

constexpr u32 kSio2Base = 0x1F808200;
constexpr u32 kSio2Size = 0x100;

// SIO2MAN gives the channel one block per queued command, counted in words.
constexpr u32 blockBytesOf(u32 bcr) {
    return (bcr & 0xFFFF) * 4;
}

constexpr u32 blocksIn(u32 bcr) {
    return bcr >> 16;
}

}  // namespace

void IopMmio::attach(Memory& memory, IopKernel& kernel) {
    mMemory = &memory;
    mKernel = &kernel;
    mSio2.attach(kernel);
    memory.attach(this);
}

IopMmio::DmaChannel* IopMmio::channelFor(u32 address) {
    for (DmaChannel* channel :
         {&mSpuChannels[0], &mSpuChannels[1], &mSio2Write, &mSio2Read}) {
        if (address - channel->base < kChannelSpan) {
            return channel;
        }
    }
    return nullptr;
}

const IopMmio::DmaChannel* IopMmio::channelFor(u32 address) const {
    return const_cast<IopMmio*>(this)->channelFor(address);
}

std::optional<u16> IopMmio::readHalf(u32 address) const {
    if (address - kSpu2Base < kSpu2Size) {
        return mSpu2.readHalf(address - kSpu2Base);
    }
    const DmaChannel* channel = channelFor(address);
    if (channel == nullptr) {
        return std::nullopt;
    }
    const std::optional<u32> word = readWord(address & ~3u);
    if (!word) {
        return std::nullopt;
    }
    return static_cast<u16>((address & 2) != 0 ? *word >> 16 : *word);
}

// SIO2MAN: Sio2TransferSubmit @ 0x1c4, Sio2TransferCollectResults @ 0x310
// Both move the FIFOs a byte at a time.
std::optional<u8> IopMmio::readByte(u32 address) const {
    if (address - kSio2Base >= kSio2Size) {
        return std::nullopt;
    }
    const std::optional<u32> value = const_cast<Sio2&>(mSio2).read(address - kSio2Base);
    return value ? std::optional<u8>{static_cast<u8>(*value)} : std::nullopt;
}

bool IopMmio::writeByte(u32 address, u8 value) {
    if (address - kSio2Base >= kSio2Size) {
        return false;
    }
    return mSio2.write(address - kSio2Base, value);
}

// LIBSD programs the channel a halfword at a time, so the word the start bit
// arrives in has to be rebuilt from the half already there.
bool IopMmio::writeHalf(u32 address, u16 value) {
    if (address - kSpu2Base < kSpu2Size) {
        return mSpu2.writeHalf(address - kSpu2Base, value);
    }
    DmaChannel* channel = channelFor(address);
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
    const DmaChannel* channel = channelFor(address);
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

// A transfer moves waveform data into the sound processor's own memory, at the
// address the core's transfer register names.
void IopMmio::sendToSoundMemory(const DmaChannel& channel, u32 words) {
    std::vector<u8> bytes(static_cast<usize>(words) * 4);
    for (usize i = 0; i < bytes.size(); ++i) {
        bytes[i] = mMemory->read8(channel.madr + static_cast<u32>(i));
    }
    mSpu2.receive(&channel == &mSpuChannels[0] ? 0 : 1, bytes);
}

void IopMmio::advance(u64 cycles) {
    if (mAudio == nullptr) {
        return;
    }
    mAudioCycles += cycles;
    const u64 frames = mAudioCycles / kIopCyclesPerSample;
    mAudioCycles -= frames * kIopCyclesPerSample;

    for (u64 done = 0; done < frames; done += kRenderFrames) {
        const usize count = static_cast<usize>(std::min<u64>(kRenderFrames, frames - done));
        std::vector<s16> stereo(count * kSpuChannels);
        mSpu2.render(stereo);
        mAudio->write(stereo);
    }
}

// The device answers the queue as the start bit is written, so the channel that
// reads it back is the one write that has to look at the device afterwards.
void IopMmio::readFromSio2() {
    if ((mSio2Read.chcr & kStart) == 0 || !mSio2.hasAnswers()) {
        return;
    }
    const u32 block = blockBytesOf(mSio2Read.bcr);
    std::vector<u8> answer(static_cast<usize>(block) * blocksIn(mSio2Read.bcr));
    mSio2.readFifoOut(answer, block);
    for (usize i = 0; i < answer.size(); ++i) {
        mMemory->write8(mSio2Read.madr + static_cast<u32>(i), answer[i]);
    }
    mSio2Read.chcr &= ~kStart;
}

void IopMmio::writeToSio2(const DmaChannel& channel) {
    const u32 block = blockBytesOf(channel.bcr);
    std::vector<u8> command(static_cast<usize>(block) * blocksIn(channel.bcr));
    for (usize i = 0; i < command.size(); ++i) {
        command[i] = mMemory->read8(channel.madr + static_cast<u32>(i));
    }
    mSio2.writeFifoIn(command, block);
}

// LIBSD waits on the channel's interrupt rather than on the data, and pacing it
// at the rate the samples play keeps a driver that refills on it in step.
void IopMmio::startTransfer(DmaChannel& channel) {
    if (channel.base == mSio2Write.base) {
        writeToSio2(channel);
        channel.chcr &= ~kStart;
        return;
    }
    if (channel.base == mSio2Read.base) {
        return;
    }

    const u32 words = (channel.bcr & 0xFFFF) * blocksIn(channel.bcr);
    log.trace("spu dma from {:08x}, {} words, line {}", channel.madr, words, channel.line);
    sendToSoundMemory(channel, words);
    channel.chcr &= ~kStart;
    mKernel->raiseInterruptAfter(channel.line, kIopCyclesPerSample * samplesIn(words));
}

bool IopMmio::writeWord(u32 address, u32 value) {
    if (address - kSio2Base < kSio2Size) {
        const bool handled = mSio2.write(address - kSio2Base, value);
        readFromSio2();
        return handled;
    }
    DmaChannel* channel = channelFor(address);
    if (channel == nullptr) {
        return false;
    }
    switch (address - channel->base) {
        case kMadr: channel->madr = value; return true;
        case kBcr: channel->bcr = value; return true;
        case kChcr: break;
        default: return false;
    }

    channel->chcr = value;
    if ((value & kStart) != 0) {
        startTransfer(*channel);
    }
    return true;
}

void IopMmio::startVu1(u32 address) {
    throw std::runtime_error(
        std::format("the iop started a vector unit at {:#x}; it has none", address));
}

}  // namespace uee
