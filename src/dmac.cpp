#include "dmac.h"

#include "gif.h"
#include "vif.h"

#include "interrupts.h"
#include "memory.h"
#include "syscall_stubs.h"

#include <format>

namespace uee {
namespace {

// SLUS-216.21: sceGsSyncPath @ 0x4f9ccc names these.
constexpr u32 kChannelBase[dmac::kChannels] = {
    0x10008000,  // VIF0
    0x10009000,  // VIF1
    0x1000A000,  // GIF
    0x1000B000,  // from IPU
    0x1000B400,  // to IPU
    0x1000C000,  // SIF0
    0x1000C400,  // SIF1
    0x1000C800,  // SIF2
    0x1000D000,  // from scratchpad
    0x1000D400,  // to scratchpad
};

constexpr usize kSif0 = 5;
constexpr usize kSif2 = 7;
constexpr usize kFromScratchpad = 8;
constexpr usize kToScratchpad = 9;

constexpr u32 kControl = 0x00;
constexpr u32 kAddress = 0x10;
constexpr u32 kQuadwords = 0x20;
constexpr u32 kTagAddress = 0x30;
constexpr u32 kReturn0 = 0x40;
constexpr u32 kReturn1 = 0x50;
constexpr u32 kScratchpadAddress = 0x80;

// KERNEL: SifSetDChain @ 0x80006340 arms with 0x184 = STR | TIE | chain | to
// memory.
constexpr u32 kStart = 0x100;
constexpr u32 kModeShift = 2;
constexpr u32 kModeMask = 0x3;
constexpr u32 kTagInterrupt = 0x80;

constexpr u32 kNormalMode = 0;
constexpr u32 kChainMode = 1;

constexpr u32 kQuadwordBytes = 16;

constexpr usize kVif0 = 0;
constexpr usize kVif1 = 1;
constexpr usize kGif = 2;

constexpr u32 kInterleaveMode = 2;
constexpr u32 kSkipQuadword = 0x1000E030;
constexpr u32 kScratchpadQuadwordMask = 0x3FF0;

constexpr u32 kScratchpadBit = 0x80000000;
constexpr u32 kScratchpadWindow = 0x70000000;

// KERNEL: SifDmaWriteTag @ 0x80006410 writes (tagopt << 16) | quadwords and ORs
// bit 31 from the interrupt attribute, which fixes all three fields.
constexpr u32 kQuadwordCountMask = 0xFFFF;
constexpr u32 kIdShift = 28;
constexpr u32 kIdMask = 0x7;
constexpr u32 kIrq = 0x80000000;

// Each id names a site that builds it. Nothing builds a refs, so it walks as
// ref and reports that.
//   0 refe, 3 ref   KERNEL: SifDmaWriteTag @ 0x80006410
//   1 cnt, 2 next, 5 call, 7 end   SLUS-216.21: _rwDMAOpenGIFPkt @ 0x4d54e0
//   6 ret   SLUS-216.21: DMADataFillTags @ 0x49dc70
constexpr u32 kReferenceAndEnd = 0;
constexpr u32 kCount = 1;
constexpr u32 kNext = 2;
constexpr u32 kReference = 3;
constexpr u32 kReferenceStalled = 4;
constexpr u32 kCall = 5;
constexpr u32 kReturn = 6;
constexpr u32 kEnd = 7;

struct Tag {
    u32 quadwords = 0;
    u32 id = 0;
    bool interrupt = false;
    u32 address = 0;
};

Tag readTag(Memory& memory, u32 address) {
    const u32 low = memory.read32(address);
    return Tag{low & kQuadwordCountMask, (low >> kIdShift) & kIdMask, (low & kIrq) != 0,
               memory.read32(address + 4)};
}

u32 resolve(u32 address) {
    return (address & kScratchpadBit) != 0 ? kScratchpadWindow | (address & 0x3FFFu)
                                           : address & 0x7FFFFFFFu;
}

}  // namespace

void Dmac::attach(Memory& memory, Interrupts& interrupts, Gif& gif, Vif& vif0, Vif& vif1) {
    mMemory = &memory;
    mGif = &gif;
    mVif[0] = &vif0;
    mVif[1] = &vif1;
    mInterrupts = &interrupts;
}

std::optional<u32> Dmac::read(u32 address) const {
    if (address == kSkipQuadword) {
        return mSkipQuadword;
    }
    if (address == dmac::kEnableRead || address == dmac::kEnableWrite) {
        return mEnable;
    }
    for (usize i = 0; i < dmac::kChannels; ++i) {
        const u32 offset = address - kChannelBase[i];
        if (address < kChannelBase[i] || offset > kScratchpadAddress) {
            continue;
        }
        switch (offset) {
            case kControl: return mChannels[i].control;
            case kAddress: return mChannels[i].address;
            case kQuadwords: return mChannels[i].quadwords;
            case kTagAddress: return mChannels[i].tagAddress;
            case kReturn0: return mChannels[i].returnAddress[0];
            case kReturn1: return mChannels[i].returnAddress[1];
            case kScratchpadAddress: return mChannels[i].scratchpadAddress;
            default: return std::nullopt;
        }
    }
    return std::nullopt;
}

bool Dmac::write(u32 address, u32 value) {
    if (address == kSkipQuadword) {
        mSkipQuadword = value;
        return true;
    }
    if (address == dmac::kEnableWrite) {
        const bool resumed = (mEnable & dmac::kSuspended) != 0 &&
                             (value & dmac::kSuspended) == 0;
        mEnable = value;
        if (resumed) {
            for (usize i = 0; i < dmac::kChannels; ++i) {
                if ((mChannels[i].control & kStart) != 0) {
                    start(i);
                }
            }
        }
        return true;
    }
    for (usize i = 0; i < dmac::kChannels; ++i) {
        const u32 offset = address - kChannelBase[i];
        if (address < kChannelBase[i] || offset > kScratchpadAddress) {
            continue;
        }
        switch (offset) {
            case kControl:
                mChannels[i].control = value;
                if ((value & kStart) != 0) {
                    start(i);
                }
                return true;
            case kAddress:
                mChannels[i].address = (i == kFromScratchpad || i == kToScratchpad)
                                           ? (value & ~kScratchpadBit)
                                           : value;
                return true;
            case kQuadwords: mChannels[i].quadwords = value; return true;
            case kTagAddress: mChannels[i].tagAddress = value; return true;
            case kReturn0: mChannels[i].returnAddress[0] = value; return true;
            case kReturn1: mChannels[i].returnAddress[1] = value; return true;
            case kScratchpadAddress:
                mChannels[i].scratchpadAddress = value & kScratchpadQuadwordMask;
                return true;
            default: return false;
        }
    }
    return false;
}

void Dmac::start(usize index) {
    if ((mEnable & dmac::kSuspended) != 0) {
        return;
    }
    // SIF belongs to the Sif side. SIF0 stays armed, which is what the guest
    // tests before arming it again.
    if (index >= kSif0 && index <= kSif2) {
        if (index != kSif0) {
            mChannels[index].control &= ~kStart;
        }
        return;
    }
    Channel& channel = mChannels[index];
    const u32 mode = (channel.control >> kModeShift) & kModeMask;
    if (mode == kChainMode) {
        followChain(index);
    } else if (mode == kInterleaveMode) {
        interleave(index);
    } else if (mode == kNormalMode) {
        transfer(index, channel.address, channel.quadwords);
        channel.address += channel.quadwords * kQuadwordBytes;
        channel.quadwords = 0;
    } else {
        reportStub(std::format("dma channel {} mode {}", index, mode));
    }

    channel.control &= ~kStart;
    mInterrupts->raiseDmac(static_cast<u32>(index));
}

// Only the scratchpad channels have somewhere to put the data.
// Interleave walks a run of quadwords then steps the address over a gap, which
// is how the scratchpad channels move a rectangle out of a larger image.
// Interleave walks a run of quadwords then steps over a gap, which is how the
// scratchpad channels move a rectangle out of a larger image. The walking
// cursor is local because the channel's own address is left where it started.
void Dmac::interleave(usize index) {
    Channel& channel = mChannels[index];
    const u32 run = (mSkipQuadword >> 16) & 0xFFu;
    const u32 skip = mSkipQuadword & 0xFFu;
    if (run == 0) {
        transfer(index, channel.address, channel.quadwords);
        channel.address += channel.quadwords * kQuadwordBytes;
        channel.quadwords = 0;
        return;
    }
    for (u32 moved = 0; channel.quadwords != 0; --channel.quadwords) {
        transfer(index, channel.address, 1);
        channel.address += kQuadwordBytes;
        if (++moved == run) {
            moved = 0;
            channel.address += skip * kQuadwordBytes;
        }
    }
}

void Dmac::transfer(usize index, u32 address, u32 quadwords) {
    if (index == kVif0 || index == kVif1 || index == kGif) {
        u32 source = resolve(address);
        for (u32 moved = 0; moved < quadwords; ++moved, source += kQuadwordBytes) {
            const u64 low = mMemory->read64(source);
            const u64 high = mMemory->read64(source + 8);
            if (index == kGif) {
                mGif->writeQuadword(low, high);
            } else {
                mVif[index]->writeQuadword(low, high);
            }
        }
        return;
    }
    if (index != kFromScratchpad && index != kToScratchpad) {
        return;
    }
    Channel& channel = mChannels[index];
    u32 main = resolve(address);
    for (u32 moved = 0; moved < quadwords; ++moved) {
        const u32 scratchpad =
            kScratchpadWindow | (channel.scratchpadAddress & kScratchpadQuadwordMask);
        const u32 from = index == kFromScratchpad ? scratchpad : main;
        const u32 to = index == kFromScratchpad ? main : scratchpad;
        for (u32 i = 0; i < kQuadwordBytes; i += 8) {
            mMemory->write64(to + i, mMemory->read64(from + i));
        }
        main += kQuadwordBytes;
        channel.scratchpadAddress =
            (channel.scratchpadAddress + kQuadwordBytes) & kScratchpadQuadwordMask;
    }
}

// The tag lands in the upper half of the control register, where the guest
// reads back what the transfer finished on.
void Dmac::followChain(usize index) {
    Channel& channel = mChannels[index];
    for (u32 step = 0; step < 0x10000; ++step) {
        const Tag tag = readTag(*mMemory, resolve(channel.tagAddress));
        channel.control = (channel.control & 0xFFFFu) |
                          (mMemory->read32(resolve(channel.tagAddress)) & 0xFFFF0000u);

        const u32 follow = resolve(channel.tagAddress) + kQuadwordBytes;
        switch (tag.id) {
            case kReferenceAndEnd:
            case kReference:
            case kReferenceStalled:
                transfer(index, resolve(tag.address), tag.quadwords);
                channel.address = resolve(tag.address) + tag.quadwords * kQuadwordBytes;
                channel.tagAddress = follow;
                break;
            case kCount:
                transfer(index, follow, tag.quadwords);
                channel.address = follow + tag.quadwords * kQuadwordBytes;
                channel.tagAddress = channel.address;
                break;
            // Unlike the other terminators, end leaves TADR on itself rather
            // than past it.
            case kEnd:
                transfer(index, follow, tag.quadwords);
                channel.address = follow + tag.quadwords * kQuadwordBytes;
                break;
            case kNext:
                transfer(index, follow, tag.quadwords);
                channel.address = follow + tag.quadwords * kQuadwordBytes;
                channel.tagAddress = resolve(tag.address);
                break;
            case kCall:
                transfer(index, follow, tag.quadwords);
                channel.address = follow + tag.quadwords * kQuadwordBytes;
                channel.returnAddress[channel.returnDepth] =
                    follow + tag.quadwords * kQuadwordBytes;
                channel.returnDepth = (channel.returnDepth + 1) % dmac::kReturnDepth;
                channel.tagAddress = resolve(tag.address);
                break;
            case kReturn:
                transfer(index, follow, tag.quadwords);
                channel.address = follow + tag.quadwords * kQuadwordBytes;
                channel.returnDepth = (channel.returnDepth + dmac::kReturnDepth - 1) % dmac::kReturnDepth;
                channel.tagAddress = channel.returnAddress[channel.returnDepth];
                break;
            default:
                reportStub(std::format("dma tag id {}", tag.id));
                return;
        }
        if (tag.id == kReferenceStalled) {
            reportStub("dma tag refs walked as ref");
        }

        channel.quadwords = 0;
        const bool ends = tag.id == kReferenceAndEnd || tag.id == kEnd;
        const bool interrupts = tag.interrupt && (channel.control & kTagInterrupt) != 0;
        if (ends || interrupts) {
            return;
        }
    }
    reportStub(std::format("dma chain on channel {} does not end", index));
}

}  // namespace uee
