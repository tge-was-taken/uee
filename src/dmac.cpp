#include "log.h"
#include "dmac.h"

#include "gif.h"
#include "vif.h"

#include "interrupts.h"
#include "memory.h"

#include <format>
#include <vector>

namespace uee {
namespace {

LoggerChannel log("dmac");


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
constexpr u32 kModeBits = kModeMask << kModeShift;
constexpr u32 kTagInterrupt = 0x80;

constexpr u32 kNormalMode = 0;
constexpr u32 kChainMode = 1;

constexpr u32 kQuadwordBytes = 16;

constexpr usize kVif0 = 0;
constexpr usize kVif1 = 1;
constexpr usize kGif = 2;
constexpr usize kToIpu = 4;

constexpr u32 kPath3 = 3;

// CHCR: the channel sends each tag's codes to the unit ahead of the data.
constexpr u32 kTagTransfer = 1u << 6;

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

// Nothing builds a refs, so it walks as ref and reports that.
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

void Dmac::attach(Memory& memory, Interrupts& interrupts, Gif& gif, Vif& vif0, Vif& vif1,
                  Ipu& ipu) {
    mMemory = &memory;
    mGif = &gif;
    mVif[0] = &vif0;
    mVif[1] = &vif1;
    mIpu = &ipu;
    mInterrupts = &interrupts;
}

std::optional<u32> Dmac::read(u32 address) {
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
            // A read is what retires a finished channel, and it reports what
            // the register held before that.
            case kControl: {
                const u32 control = mChannels[i].control;
                retire(mChannels[i]);
                return control;
            }
            case kAddress: {
                const u32 address = mChannels[i].address;
                retire(mChannels[i]);
                return address;
            }
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
        // A run and a gap, one byte each; the rest of the register keeps nothing.
        mSkipQuadword = value & 0x00FF00FFu;
        return true;
    }
    if (address == dmac::kEnableWrite) {
        const bool resumed = (mEnable & dmac::kSuspended) != 0 &&
                             (value & dmac::kSuspended) == 0;
        // Only the suspend bit is a guest's to set; the rest of the register
        // reports what the machine came up with.
        mEnable = (mEnable & ~dmac::kSuspended) | (value & dmac::kSuspended);
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
        // A finished transfer writes its address back before the guest's own
        // write to the same register, not after it.
        retire(mChannels[i]);
        switch (offset) {
            case kControl:
                {
                    const u32 mask = controlMask(i);
                    const u32 mode = settledMode(i, (value >> kModeShift) & kModeMask);
                    const u32 kept = (value & mask & ~kModeBits) |
                                     (mode << kModeShift) | (value & kStart);
                    mChannels[i].control = (mChannels[i].control & ~(mask | kStart)) | kept;
                }
                if ((value & kStart) != 0) {
                    log.trace("channel {} started, control {:08x}", i, value);
                    start(i);
                }
                return true;
            case kAddress: mChannels[i].address = value & addressMask(i); return true;
            case kQuadwords: mChannels[i].quadwords = value & kQuadwordCountMask; return true;
            case kTagAddress:
                mChannels[i].tagAddress = value & tagAddressMask(i);
                return true;
            case kReturn0: mChannels[i].returnAddress[0] = value; return true;
            case kReturn1: mChannels[i].returnAddress[1] = value; return true;
            case kScratchpadAddress:
                mChannels[i].scratchpadAddress =
                    value & (isScratchpadChannel(i) ? kScratchpadQuadwordMask : 0u);
                return true;
            default: return false;
        }
    }
    return false;
}

// Writing all ones and reading back is what these came from: every address is
// quadword aligned, the bit that names the scratchpad belongs only to the
// channels that cannot reach it in their own address, and a channel with no
// list or no scratchpad keeps nothing.
void Dmac::installBootChannels() {
    mChannels[kVif0].control = 0x00000001;
    mChannels[kGif].control = 0x00000001;
    mChannels[kToScratchpad].control = 0x00000001;
    mChannels[kSif0].control = 0x00000184;
    mChannels[kSif0].address = 0x00158C00;
    mChannels[kSif0 + 1].control = 0x00000085;
    mChannels[kSif0 + 1].address = 0x00021B10;
    mChannels[kSif0 + 1].tagAddress = 0x00021390;
}

bool Dmac::isScratchpadChannel(usize index) {
    return index == kFromScratchpad || index == kToScratchpad;
}

u32 Dmac::addressMask(usize index) {
    return isScratchpadChannel(index) ? 0x7FFFFFF0u : 0xFFFFFFF0u;
}

// Writing all ones and reading back is what these came from: each channel keeps
// only the transfer bits it has a use for.
u32 Dmac::controlMask(usize index) {
    switch (index) {
        case kVif0:
        case kGif: return 0xFFFF00F4u | kModeBits;
        case kVif1: return 0xFFFF00F0u | kModeBits;
        case kFromScratchpad: return 0xFFFF0088u | kModeBits;
        case kToScratchpad: return 0xFFFF00C8u | kModeBits;
        default: return 0xFFFF00FEu;
    }
}

// The fourth mode has no meaning, and each channel answers a write of it with
// one of its own rather than keeping it.
u32 Dmac::settledMode(usize index, u32 mode) {
    if (mode != 3) {
        return mode;
    }
    switch (index) {
        case kVif0:
        case kGif: return kChainMode;
        case kFromScratchpad:
        case kToScratchpad: return kInterleaveMode;
        case kVif1: return kNormalMode;
        default: return mode;
    }
}

u32 Dmac::tagAddressMask(usize index) {
    return index == kFromScratchpad ? 0u : 0xFFFFFFF0u;
}

void Dmac::retire(Channel& channel) {
    if (!channel.finishing) {
        return;
    }
    channel.finishing = false;
    channel.control &= ~kStart;
    channel.address = channel.retiredAddress;
}

void Dmac::start(usize index) {
    // The scratchpad channels burst rather than wait for the arbiter, so the
    // suspend the rest of them honour does not hold them.
    if ((mEnable & dmac::kSuspended) != 0 && !isScratchpadChannel(index)) {
        return;
    }
    // SIF0 stays armed, which the guest tests before arming it again.
    if (index >= kSif0 && index <= kSif2) {
        if (index != kSif0) {
            // The transfer to the iop is over as soon as it is armed, so the
            // channel reports it the way a completed one does.
            mChannels[index].control &= ~kStart;
            mInterrupts->raiseDmac(static_cast<u32>(index));
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
        // A count of zero is the whole of the field's range rather than nothing.
        // A count of zero is the whole of the field's range rather than nothing.
        const u32 count = channel.quadwords == 0 ? 0x10000u : channel.quadwords;
        channel.retiredAddress = transfer(index, channel.address, count);
        channel.quadwords = 0;
    } else {
        log.stub("dma channel {} mode {}", index, mode);
    }

    if (mode != kNormalMode) {
        channel.retiredAddress = channel.address;
    }
    channel.finishing = true;
    // A suspended controller never granted the transfer, so there is no
    // running channel for the guest to catch.
    if ((mEnable & dmac::kSuspended) != 0) {
        retire(channel);
    }
    if (index == kToIpu) {
        log.trace("ipu fed {} bytes", mIpu->getStream().size());
    }
    log.trace("channel {} finished, mode {}", index, mode);
    mInterrupts->raiseDmac(static_cast<u32>(index));
}

// A run then a gap, moving a rectangle out of a larger image.
void Dmac::interleave(usize index) {
    Channel& channel = mChannels[index];
    const u32 run = (mSkipQuadword >> 16) & 0xFFu;
    const u32 skip = mSkipQuadword & 0xFFu;
    if (run == 0) {
        channel.address = transfer(index, channel.address, channel.quadwords);
        channel.quadwords = 0;
        return;
    }
    for (u32 moved = 0; channel.quadwords != 0; --channel.quadwords) {
        channel.address = transfer(index, channel.address, 1);
        if (++moved == run) {
            moved = 0;
            channel.address += skip * kQuadwordBytes;
        }
    }
}

// A tag's upper half holds two VIFcodes, read before the data the tag names.
void Dmac::sendTagCodes(usize index, u32 tagAddress) {
    if ((index != kVif0 && index != kVif1) ||
        (mChannels[index].control & kTagTransfer) == 0) {
        return;
    }
    mVif[index]->writeTag(mMemory->read64(tagAddress + 8));
}

// Answers where the address ended, which is not always where it started plus
// the length: a ring wraps it.
u32 Dmac::transfer(usize index, u32 address, u32 quadwords) {
    if (index == kVif0 || index == kVif1 || index == kGif) {
        if (index == kGif) {
            mGif->beginPath(kPath3);
        }
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
        if (index == kGif) {
            mGif->endPath();
        }
        return address + quadwords * kQuadwordBytes;
    }
    if (index == kToIpu) {
        std::vector<u8> bytes(static_cast<usize>(quadwords) * kQuadwordBytes);
        const u32 source = resolve(address);
        for (usize i = 0; i < bytes.size(); ++i) {
            bytes[i] = mMemory->read8(source + static_cast<u32>(i));
        }
        mIpu->receive(bytes);
        return address + quadwords * kQuadwordBytes;
    }
    if (index != kFromScratchpad && index != kToScratchpad) {
        return address + quadwords * kQuadwordBytes;
    }
    Channel& channel = mChannels[index];
    // The ring size is a mask, so the address it wraps is the offset into it.
    const u32 ringSize = mInterrupts->ringSize();
    const u32 ringBase = mInterrupts->ringBase();
    u32 cursor = address;
    for (u32 moved = 0; moved < quadwords; ++moved) {
        const u32 main = resolve(cursor);
        const u32 scratchpad =
            kScratchpadWindow | (channel.scratchpadAddress & kScratchpadQuadwordMask);
        const u32 from = index == kFromScratchpad ? scratchpad : main;
        const u32 to = index == kFromScratchpad ? main : scratchpad;
        for (u32 i = 0; i < kQuadwordBytes; i += 8) {
            mMemory->poke64(to + i, mMemory->peek64(from + i));
        }
        cursor = ringSize != 0 ? ringBase + ((cursor + kQuadwordBytes) & ringSize)
                               : cursor + kQuadwordBytes;
        channel.scratchpadAddress =
            (channel.scratchpadAddress + kQuadwordBytes) & kScratchpadQuadwordMask;
    }
    return cursor;
}

// The tag lands in the upper half of the control register.
void Dmac::followChain(usize index) {
    Channel& channel = mChannels[index];
    for (u32 step = 0; step < 0x10000; ++step) {
        const Tag tag = readTag(*mMemory, resolve(channel.tagAddress));
        channel.control = (channel.control & 0xFFFFu) |
                          (mMemory->read32(resolve(channel.tagAddress)) & 0xFFFF0000u);

        const u32 follow = resolve(channel.tagAddress) + kQuadwordBytes;
        sendTagCodes(index, resolve(channel.tagAddress));

        // The scratchpad channels have no address stack, so a tag that names
        // one is loaded and then goes no further.
        if (isScratchpadChannel(index) && (tag.id == kCall || tag.id == kReturn)) {
            channel.address = follow;
            channel.quadwords = tag.quadwords;
            return;
        }

        switch (tag.id) {
            case kReferenceAndEnd:
            case kReference:
            case kReferenceStalled:
                transfer(index, resolve(tag.address), tag.quadwords);
                // A tag with nothing to move never loads the address it names.
                channel.address = tag.quadwords == 0
                                      ? follow
                                      : resolve(tag.address) + tag.quadwords * kQuadwordBytes;
                channel.tagAddress = follow;
                break;
            case kCount:
                transfer(index, follow, tag.quadwords);
                channel.address = follow + tag.quadwords * kQuadwordBytes;
                channel.tagAddress = channel.address;
                break;
            // end leaves TADR on itself, unlike the other terminators.
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
                log.stub("dma tag id {}", tag.id);
                return;
        }
        if (tag.id == kReferenceStalled) {
            log.stub("dma tag refs walked as ref");
        }

        channel.quadwords = 0;
        const bool ends = tag.id == kReferenceAndEnd || tag.id == kEnd;
        const bool interrupts = tag.interrupt && (channel.control & kTagInterrupt) != 0;
        if (ends || interrupts) {
            return;
        }
    }
    log.stub("dma chain on channel {} does not end", index);
}

}  // namespace uee
