#include "mmio.h"

namespace uee {
namespace {

// The ranges are disjoint, so an address decodes to one block.
enum class Block { None, Timers, Dmac, Interrupts, Sbus };

struct Range {
    u32 base = 0;
    u32 size = 0;
    Block block = Block::None;
};

constexpr Range kRanges[] = {
    {0x10000000, 0x2000, Block::Timers},
    {0x10008000, 0x6000, Block::Dmac},
    {0x1000E000, 0x0100, Block::Interrupts},
    {0x1000F000, 0x0100, Block::Interrupts},
    {0x1000F200, 0x0100, Block::Sbus},
    {0x1000F520, 0x0004, Block::Dmac},
    {0x1000F590, 0x0004, Block::Dmac},
};

constexpr Block blockOf(u32 address) {
    for (const Range& range : kRanges) {
        if (address - range.base < range.size) {
            return range.block;
        }
    }
    return Block::None;
}

}  // namespace

std::optional<u32> Mmio::read(u32 address) const {
    switch (blockOf(address)) {
        case Block::Timers: return mTimers.read(address);
        case Block::Dmac: return mDmac.read(address);
        case Block::Interrupts: return mInterrupts.read(address);
        case Block::Sbus: return mSbus.read(address);
        case Block::None: return std::nullopt;
    }
    return std::nullopt;
}

bool Mmio::write(u32 address, u32 value) {
    switch (blockOf(address)) {
        case Block::Timers: return mTimers.write(address, value);
        case Block::Dmac: return mDmac.write(address, value);
        case Block::Interrupts: return mInterrupts.write(address, value);
        case Block::Sbus: return mSbus.write(address, value);
        case Block::None: return false;
    }
    return false;
}

}  // namespace uee
