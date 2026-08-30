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

// LIBSD: SpuInitCores @ 0x29ec waits on bit 7 of STATX before it touches a core
std::optional<u16> IopMmio::readHalf(u32 address) const {
    if (address == kCore0Statx || address == kCore1Statx) {
        return kTransferReady;
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

// SIO2MAN moves the FIFOs a byte at a time: Sio2TransferSubmit @ 0x1c4 stores
// each command byte with sb, and Sio2TransferCollectResults @ 0x310 reads the
// answer back with lbu.
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

void IopMmio::sendToAudio(const DmaChannel& channel, u32 words) {
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

// The device answers the queue as the start bit is written, so the channel that
// reads it back is the one write that has to look at the device afterwards.
void IopMmio::readFromSio2() {
    const u32 bytes = (mSio2Read.bcr & 0xFFFF) * (mSio2Read.bcr >> 16) * 4;
    if ((mSio2Read.chcr & kStart) == 0 || mSio2.pendingOut() < bytes) {
        return;
    }
    std::vector<u8> answer(bytes);
    mSio2.readFifoOut(answer);
    for (usize i = 0; i < answer.size(); ++i) {
        mMemory->write8(mSio2Read.madr + static_cast<u32>(i), answer[i]);
    }
    mSio2Read.chcr &= ~kStart;
}

void IopMmio::writeToSio2(const DmaChannel& channel, u32 bytes) {
    std::vector<u8> command(bytes);
    for (usize i = 0; i < command.size(); ++i) {
        command[i] = mMemory->read8(channel.madr + static_cast<u32>(i));
    }
    mSio2.writeFifoIn(command);
}

// The transfer to sound RAM is not modelled: the samples go to the host and
// LIBSD waits on the interrupt, not on the data.
void IopMmio::startTransfer(DmaChannel& channel) {
    const u32 words = (channel.bcr & 0xFFFF) * (channel.bcr >> 16);
    if (channel.base == mSio2Write.base) {
        writeToSio2(channel, words * 4);
        channel.chcr &= ~kStart;
        return;
    }
    if (channel.base == mSio2Read.base) {
        return;
    }
    log.trace("spu dma from {:08x}, {} words, line {}", channel.madr, words, channel.line);
    sendToAudio(channel, words);
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

}  // namespace uee
