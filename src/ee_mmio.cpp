#include "ee_mmio.h"

#include "memory.h"

#include <array>

namespace uee {
namespace {

constexpr u32 kIpuBase = 0x10002000;

// The ranges are disjoint, so an address decodes to one block.
enum class Block { None, Timers, Dmac, Interrupts, Sbus, Gif, Gs, Ipu, Vif0, Vif1 };

struct Range {
    u32 base = 0;
    u32 size = 0;
    Block block = Block::None;
};

constexpr Range kRanges[] = {
    {kTimerBase, 0x2000, Block::Timers},
    {0x10008000, 0x6000, Block::Dmac},
    // Inside the interrupt window's span, so it is named first.
    {0x1000E030, 0x0004, Block::Dmac},
    {kDmacBase, 0x0100, Block::Interrupts},
    {kIntcBase, 0x0100, Block::Interrupts},
    {0x1000F200, 0x0100, Block::Sbus},
    {0x1000F520, 0x0004, Block::Dmac},
    {0x1000F590, 0x0004, Block::Dmac},
    {0x10002000, 0x0100, Block::Ipu},
    {0x10003000, 0x0100, Block::Gif},
    {0x10003800, 0x0180, Block::Vif0},
    {0x10003C00, 0x0180, Block::Vif1},
    {0x10004000, 0x1000, Block::Vif0},
    {0x10005000, 0x1000, Block::Vif1},
    {kGsPrivBase, kGsPrivSize, Block::Gs},
};

// One entry per 16 bytes of the window, which is the finest any range splits it.
constexpr u32 kUnitShift = 4;
constexpr usize kUnits = kMmioSize >> kUnitShift;

constexpr std::array<Block, kUnits> mapWindow() {
    std::array<Block, kUnits> blocks{};
    for (const Range& range : kRanges) {
        if (range.base - kMmioBase >= kMmioSize) {
            continue;
        }
        const usize first = (range.base - kMmioBase) >> kUnitShift;
        const usize count = (range.size + (1 << kUnitShift) - 1) >> kUnitShift;
        for (usize i = first; i < first + count && i < kUnits; ++i) {
            // First named wins, so a range inside another's span still reaches
            // its own block.
            if (blocks[i] == Block::None) {
                blocks[i] = range.block;
            }
        }
    }
    return blocks;
}

constexpr std::array<Block, kUnits> kBlocks = mapWindow();

constexpr Block blockOf(u32 address) {
    if (address - kGsPrivBase < kGsPrivSize) {
        return Block::Gs;
    }
    if (address - kMmioBase >= kMmioSize) {
        return Block::None;
    }
    return kBlocks[(address - kMmioBase) >> kUnitShift];
}

}  // namespace

std::optional<u32> EeMmio::read(u32 address) const {
    switch (blockOf(address)) {
        case Block::Timers: return mTimers.read(address);
        case Block::Dmac: return mDmac.read(address);
        case Block::Interrupts: return mInterrupts.read(address);
        case Block::Sbus: return mSbus.read(address);
        case Block::Gif: return mGif.read(address);
        case Block::Ipu: return mIpu.read(address - kIpuBase);
        case Block::Gs: return mGs.read(address);
        case Block::Vif0: return mVif[0].read(address);
        case Block::Vif1: return mVif[1].read(address);
        case Block::None: return std::nullopt;
    }
    return std::nullopt;
}

bool EeMmio::write(u32 address, u32 value) {
    const bool handled = [&] {
        switch (blockOf(address)) {
            case Block::Timers: return mTimers.write(address, value);
            case Block::Dmac: return mDmac.write(address, value);
            case Block::Interrupts: return mInterrupts.write(address, value);
            case Block::Sbus: return mSbus.write(address, value);
            case Block::Gif: return mGif.write(address, value);
            case Block::Ipu: return mIpu.write(address - kIpuBase, value);
            case Block::Gs: return mGs.write(address, value);
            case Block::Vif0: return mVif[0].write(address, value);
            case Block::Vif1: return mVif[1].write(address, value);
            case Block::None: return false;
        }
        return false;
    }();
    if (handled) {
        refreshCop0Condition();
    }
    return handled;
}

}  // namespace uee
