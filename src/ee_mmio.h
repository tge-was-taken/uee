#pragma once

#include "dmac.h"
#include "gif.h"
#include "gs.h"
#include "ee_context.h"
#include "host.h"
#include "interrupts.h"
#include "ipu.h"
#include "memory.h"
#include "r5900.h"
#include "mmio.h"
#include "sbus.h"
#include "timers.h"
#include "vif.h"
#include "vu.h"
#include "types.h"

#include <optional>
#include <vector>

namespace uee {

// The ranges are disjoint, so an address decodes to one block.
constexpr u32 kIpuBase = 0x10002000;

enum class EeMmioBlock { None, Timers, Dmac, Interrupts, Sbus, Gif, Gs, Ipu, Vif0, Vif1 };

struct RegisterRange {
    u32 base = 0;
    u32 size = 0;
    EeMmioBlock block = EeMmioBlock::None;
};

constexpr RegisterRange kRanges[] = {
    {kTimerBase, 0x2000, EeMmioBlock::Timers},
    {0x10008000, 0x6000, EeMmioBlock::Dmac},
    // The skip quadword register sits inside the interrupt window's span, so it
    // is named first for the search to reach it.
    {0x1000E030, 0x0004, EeMmioBlock::Dmac},
    {kDmacBase, 0x0100, EeMmioBlock::Interrupts},
    {kIntcBase, 0x0100, EeMmioBlock::Interrupts},
    {0x1000F200, 0x0100, EeMmioBlock::Sbus},
    {0x1000F520, 0x0004, EeMmioBlock::Dmac},
    {0x1000F590, 0x0004, EeMmioBlock::Dmac},
    {kIpuBase, 0x0100, EeMmioBlock::Ipu},
    {0x10003000, 0x0100, EeMmioBlock::Gif},
    {0x10003800, 0x0180, EeMmioBlock::Vif0},
    {0x10003C00, 0x0180, EeMmioBlock::Vif1},
    {0x10004000, 0x1000, EeMmioBlock::Vif0},
    {0x10005000, 0x1000, EeMmioBlock::Vif1},
    {kGsPrivBase, kGsPrivSize, EeMmioBlock::Gs},
};

constexpr EeMmioBlock mmioBlockOf(u32 address) {
    for (const RegisterRange& range : kRanges) {
        if (address - range.base < range.size) {
            return range.block;
        }
    }
    return EeMmioBlock::None;
}

// The registers in the 0x10000000 window that this runtime models. Everything
// else in the window stays plain memory.
class EeMmio : public Mmio {
public:
    std::span<const MmioSegment> segments() const override {
        static constexpr MmioSegment kSegments[] = {{kMmioBase, kMmioSize},
                                                  {kGsPrivBase, kGsPrivSize}};
        return kSegments;
    }

    std::optional<u32> readWord(u32 address) override { return read(address); }
    bool writeWord(u32 address, u32 value) override { return write(address, value); }
    // Only the GS has a 64-bit register modelled; everything else splits.
    std::optional<u64> readDouble(u32 address) const override { return mGs.readDouble(address); }
    bool writeDouble(u32 address, u64 value) override {
        if (!mGs.writeDouble(address, value)) {
            return false;
        }
        refreshCop0Condition();
        return true;
    }

    void attach(Memory& memory, EeContext& ee) {
        mCpu = &ee.cpu;
        mMemory = &memory;
        mGif.attach(memory, mGs);
        mVif[0].attach(memory, mGif, 0, &ee.vu0);
        mVif[1].attach(memory, mGif, 1, &mVu1);
        mDmac.attach(memory, mInterrupts, mGif, mVif[0], mVif[1], mIpu);
        mDmac.installBootChannels();
        refreshCop0Condition();
    }

    // Restated wherever a register feeding it, or the file holding it, moved.
    void refreshCop0Condition() {
        if (mCpu != nullptr) {
            mCpu->cop0condition = mInterrupts.conditionZero();
        }
    }

    std::optional<u32> read(u32 address);
    bool write(u32 address, u32 value);

    void startVu1(u32 address) override;

    // The block an address decodes to is a constant wherever the translator
    // resolved the address, so the search that finds it need not happen twice.
    std::optional<u32> readFrom(EeMmioBlock block, u32 address) {
        switch (block) {
            case EeMmioBlock::Timers: return mTimers.read(address);
            case EeMmioBlock::Dmac: return mDmac.read(address);
            case EeMmioBlock::Interrupts: return mInterrupts.read(address);
            case EeMmioBlock::Sbus: return mSbus.read(address);
            case EeMmioBlock::Gif: return mGif.read(address);
            case EeMmioBlock::Gs: return mGs.read(address);
            case EeMmioBlock::Ipu: return mIpu.read(address - kIpuBase);
            case EeMmioBlock::Vif0: return mVif[0].read(address);
            case EeMmioBlock::Vif1: return mVif[1].read(address);
            case EeMmioBlock::None: return std::nullopt;
        }
        return std::nullopt;
    }

    // CPCOND0 follows the DMA controller and the interrupt registers, so a
    // write anywhere else cannot have moved it.
    static constexpr bool feedsCop0Condition(EeMmioBlock block) {
        return block == EeMmioBlock::Dmac || block == EeMmioBlock::Interrupts ||
               block == EeMmioBlock::None;
    }

    template <EeMmioBlock block>
    bool writeTo(u32 address, u32 value) {
        const bool handled = writeOnly(block, address, value);
        if (handled && feedsCop0Condition(block)) {
            refreshCop0Condition();
        }
        return handled;
    }

    bool writeTo(EeMmioBlock block, u32 address, u32 value) {
        const bool handled = writeOnly(block, address, value);
        if (handled) {
            refreshCop0Condition();
        }
        return handled;
    }

    Timers& timers() { return mTimers; }
    Interrupts& interrupts() { return mInterrupts; }
    Sbus& sbus() { return mSbus; }
    Dmac& dmac() { return mDmac; }
    Gif& gif() { return mGif; }
    Ipu& ipu() { return mIpu; }
    Gs& gs() { return mGs; }
    Vu& vu1() { return mVu1; }

    void attachGsHandler(GsHandler* handler);

    // The field the unit moves to, and the registers as they stand at it, are
    // one event to anything watching the stream.
    void startVblank();
    std::vector<u8> privilegedRegisters();
    Vif& vif(u32 unit) { return mVif[unit]; }
    const Sbus& sbus() const { return mSbus; }

private:
    bool writeOnly(EeMmioBlock block, u32 address, u32 value) {
        switch (block) {
            case EeMmioBlock::Timers: return mTimers.write(address, value);
            case EeMmioBlock::Dmac: return mDmac.write(address, value);
            case EeMmioBlock::Interrupts: return mInterrupts.write(address, value);
            case EeMmioBlock::Sbus: return mSbus.write(address, value);
            case EeMmioBlock::Gif: return mGif.write(address, value);
            case EeMmioBlock::Gs: return mGs.write(address, value);
            case EeMmioBlock::Ipu: return mIpu.write(address - kIpuBase, value);
            case EeMmioBlock::Vif0: return mVif[0].write(address, value);
            case EeMmioBlock::Vif1: return mVif[1].write(address, value);
            case EeMmioBlock::None: return false;
        }
        return false;
    }

    Timers mTimers;
    Interrupts mInterrupts;
    Sbus mSbus;
    Gif mGif;
    Gs mGs;
    Vif mVif[2];
    Ipu mIpu;
    Dmac mDmac;
    // Not part of the context an interrupt saves: VU1 keeps running its own
    // microprogram across one.
    Vu mVu1;
    GsHandler* mHandler = nullptr;
    Memory* mMemory = nullptr;
    R5900* mCpu = nullptr;
};

// The fold and the decoding block already worked out, so a polled read costs
// only the device's own.
template <EeMmioBlock block>
inline u32 readMmio32(EeMmio& mmio, Memory& memory, u32 folded) {
    if (const std::optional<u32> value = mmio.readFrom(block, folded)) {
        return *value;
    }
    return memory.readMmioBacking32(folded);
}

template <EeMmioBlock block>
inline u64 readMmio64(EeMmio& mmio, Memory& memory, u32 folded) {
    return readMmio32<block>(mmio, memory, folded) |
           (static_cast<u64>(readMmio32<block>(mmio, memory, folded + 4)) << 32);
}

template <EeMmioBlock block>
inline void writeMmio32(EeMmio& mmio, Memory& memory, u32 folded, u32 value) {
    if (mmio.template writeTo<block>(folded, value)) {
        return;
    }
    memory.writeMmioBacking32(folded, value);
}

template <EeMmioBlock block>
inline void writeMmio64(EeMmio& mmio, Memory& memory, u32 folded, u64 value) {
    writeMmio32<block>(mmio, memory, folded, static_cast<u32>(value));
    writeMmio32<block>(mmio, memory, folded + 4, static_cast<u32>(value >> 32));
}

}  // namespace uee
