#pragma once

#include "types.h"

namespace uee {

struct Reg128 {
    u64 lo = 0;
    u64 hi = 0;

    constexpr u32 word(s32 i) const {
        return static_cast<u32>((i < 2 ? lo : hi) >> (32 * (i & 1)));
    }

    constexpr void setWord(s32 i, u32 v) {
        u64& half = (i < 2) ? lo : hi;
        const s32 shift = 32 * (i & 1);
        half = (half & ~(0xFFFFFFFFull << shift)) | (static_cast<u64>(v) << shift);
    }

    friend constexpr bool operator==(const Reg128&, const Reg128&) = default;
};

constexpr u32 low(const Reg128& r) {
    return static_cast<u32>(r.lo);
}

constexpr s64 signed64(const Reg128& r) {
    return static_cast<s64>(r.lo);
}

constexpr u16 half(const Reg128& r, s32 i) {
    return static_cast<u16>(r.word(i / 2) >> (16 * (i & 1)));
}

constexpr u8 byte(const Reg128& r, s32 i) {
    return static_cast<u8>(r.word(i / 4) >> (8 * (i & 3)));
}

constexpr Reg128 lanes(u32 w0, u32 w1, u32 w2, u32 w3) {
    return Reg128{(static_cast<u64>(w1) << 32) | w0, (static_cast<u64>(w3) << 32) | w2};
}

constexpr Reg128 fromHalves(const u16 h[8]) {
    Reg128 r{};
    for (s32 i = 0; i < 4; ++i) {
        r.setWord(i, static_cast<u32>(h[2 * i]) | (static_cast<u32>(h[2 * i + 1]) << 16));
    }
    return r;
}

constexpr Reg128 fromBytes(const u8 b[16]) {
    Reg128 r{};
    for (s32 i = 0; i < 4; ++i) {
        r.setWord(i, static_cast<u32>(b[4 * i]) | (static_cast<u32>(b[4 * i + 1]) << 8) |
                         (static_cast<u32>(b[4 * i + 2]) << 16) |
                         (static_cast<u32>(b[4 * i + 3]) << 24));
    }
    return r;
}

namespace cop0 {
constexpr u32 kPerformance = 25;
constexpr u32 kPerformanceControlMask = 0x000FFBFEu;
constexpr u32 kProcessorCycleEvent = 1;

// What the boot rom leaves in the registers a program finds before it writes
// any of them, taken from a console.
constexpr u32 kBootIndex = 0x0000002F;
constexpr u32 kBootRandom = 0x0000002F;
constexpr u32 kBootContext = 0x9D080000;
constexpr u32 kBootBadVAddr = 0x10001810;
constexpr u32 kBootEntryHi = 0xE005E000;
constexpr u32 kBootCompare = 0x00000001;
constexpr u32 kBootEpc = 0x00102998;
constexpr u32 kBootBadPAddr = 0x9EE206B0;
constexpr u32 kBootPerformance = 0x00080200;
constexpr u32 kBootReserved27 = 0x00000001;
constexpr u32 kBootTagLo = 0x00014010;
constexpr u32 kBootTagHi = 0xA5AD2A6A;
constexpr u32 kBootErrorEpc = 0x8000FD34;

// The map the boot rom installs, taken from a console: kuseg and the register
// windows, with one entry left over for a program to use.
struct TlbEntry {
    u32 high;
    u32 low0;
    u32 low1;
    u32 pageMask;
};

constexpr usize kTlbEntries = 48;

constexpr TlbEntry kBootTlb[kTlbEntries] = {
    {0x70000000, 0x8000001F, 0x0000001F, 0x00000000},
    {0xFFFF8000, 0x00001E1F, 0x00001F1F, 0x00006000},
    {0x10000000, 0x00400017, 0x00400053, 0x00000000},
    {0x10002000, 0x00400097, 0x004000D7, 0x00000000},
    {0x10004000, 0x00400117, 0x00400157, 0x00000000},
    {0x10006000, 0x00400197, 0x004001D7, 0x00000000},
    {0x10008000, 0x00400217, 0x00400257, 0x00000000},
    {0x1000A000, 0x00400297, 0x004002D7, 0x00000000},
    {0x1000C000, 0x00400313, 0x00400357, 0x00000000},
    {0x1000E000, 0x00400397, 0x004003D7, 0x00000000},
    {0x11000000, 0x00440017, 0x00440415, 0x0001E000},
    {0x12000000, 0x00480017, 0x00480415, 0x0001E000},
    {0x1E000000, 0x00780017, 0x007C0017, 0x01FFE000},
    {0x00080000, 0x0000201F, 0x0000301F, 0x0007E000},
    {0x00100000, 0x0000401F, 0x0000501F, 0x0007E000},
    {0x00180000, 0x0000601F, 0x0000701F, 0x0007E000},
    {0x00200000, 0x0000801F, 0x0000C01F, 0x001FE000},
    {0x00400000, 0x0001001F, 0x0001401F, 0x001FE000},
    {0x00600000, 0x0001801F, 0x0001C01F, 0x001FE000},
    {0x00800000, 0x0002001F, 0x0003001F, 0x007FE000},
    {0x01000000, 0x0004001F, 0x0005001F, 0x007FE000},
    {0x01800000, 0x0006001F, 0x0007001F, 0x007FE000},
    {0x20080000, 0x00002017, 0x00003017, 0x0007E000},
    {0x20100000, 0x00004017, 0x00005017, 0x0007E000},
    {0x20180000, 0x00006017, 0x00007017, 0x0007E000},
    {0x20200000, 0x00008017, 0x0000C017, 0x001FE000},
    {0x20400000, 0x00010017, 0x00014017, 0x001FE000},
    {0x20600000, 0x00018017, 0x0001C017, 0x001FE000},
    {0x20800000, 0x00020017, 0x00030017, 0x007FE000},
    {0x21000000, 0x00040017, 0x00050017, 0x007FE000},
    {0x21800000, 0x00060017, 0x00070017, 0x007FE000},
    {0x30100000, 0x0000403F, 0x0000503F, 0x0007E000},
    {0x30180000, 0x0000603F, 0x0000703F, 0x0007E000},
    {0x30200000, 0x0000803F, 0x0000C03F, 0x001FE000},
    {0x30400000, 0x0001003F, 0x0001403F, 0x001FE000},
    {0x30600000, 0x0001803F, 0x0001C03F, 0x001FE000},
    {0x30800000, 0x0002003F, 0x0003003F, 0x007FE000},
    {0x31000000, 0x0004003F, 0x0005003F, 0x007FE000},
    {0x31800000, 0x0006003F, 0x0007003F, 0x007FE000},
    {0xE004E000, 0x00000010, 0x00000010, 0x00000000},
    {0xE0050000, 0x00000010, 0x00000010, 0x00000000},
    {0xE0052000, 0x00000010, 0x00000010, 0x00000000},
    {0xE0054000, 0x00000010, 0x00000010, 0x00000000},
    {0xE0056000, 0x00000010, 0x00000010, 0x00000000},
    {0xE0058000, 0x00000010, 0x00000010, 0x00000000},
    {0xE005A000, 0x00000010, 0x00000010, 0x00000000},
    {0xE005C000, 0x00000010, 0x00000010, 0x00000000},
    {0xE005E000, 0x00000010, 0x00000010, 0x00000000},
};

constexpr u32 kIndex = 0;
constexpr u32 kRandom = 1;
constexpr u32 kEntryLo0 = 2;
constexpr u32 kEntryLo1 = 3;
constexpr u32 kPageMask = 5;
constexpr u32 kWired = 6;
constexpr u32 kCount = 9;
constexpr u32 kCompare = 11;
constexpr u32 kStatus = 12;
constexpr u32 kCause = 13;
constexpr u32 kContext = 4;
constexpr u32 kBadVAddr = 8;
constexpr u32 kEntryHi = 10;
constexpr u32 kEpc = 14;
constexpr u32 kProcessorId = 15;
constexpr u32 kConfig = 16;
// What an R5900 reports: revision 3.0 of implementation 0x2e.
constexpr u32 kR5900Id = 0x00002e30;
constexpr u32 kR5900Config = 0x00073443;
// The rom leaves all but one of the forty eight entries wired.
constexpr u32 kWiredAfterBoot = 0x1F;
constexpr u32 kBadPAddr = 23;
constexpr u32 kTagLo = 28;
constexpr u32 kTagHi = 29;
constexpr u32 kErrorEpc = 30;

constexpr u32 kInterruptEnable = 1u << 0;
constexpr u32 kExceptionLevel = 1u << 1;
constexpr u32 kErrorLevel = 1u << 2;
constexpr u32 kBootExceptionVectors = 1u << 22;

constexpr u32 kExceptionVector = 0x80000180;
constexpr u32 kBootExceptionVector = 0xBFC00380;

constexpr u32 kCauseCodeShift = 2;
constexpr u32 kCauseCodeMask = 0x1Fu << kCauseCodeShift;
constexpr u32 kSystemCall = 8;
// The counter's own interrupt, which is not one of the eight the controller
// outside the processor raises.
constexpr u32 kCounterInterrupt = 1u << 15;

// A reserved number answers with a real register rather than one of its own,
// and writing all ones and reading back is where each mask came from.
constexpr u32 alias(u32 index) {
    switch (index) {
        case 7: return kWired;
        case 17: case 18: case 19: return kConfig;
        case 20: case 21: case 22: return 23;
        case 31: return kProcessorId;
        default: return index;
    }
}

constexpr u32 writeMask(u32 index) {
    switch (index) {
        case kIndex: return 0x8000003Fu;
        case kContext: return 0xFF800000u;
        case kEntryLo0: return 0x83FFFFFFu;
        case kEntryLo1: return 0x03FFFFFFu;
        case kPageMask: return 0x01FFE000u;
        case 1: case 7: case 15: case kConfig:
        case 17: case 18: case 19: case 20: case 21: case 22: case 23:
        case 26: case 27: case 31:
            return 0u;
        default: return 0xFFFFFFFFu;
    }
}
constexpr u32 kBreakpoint = 9;
}  // namespace cop0

struct R5900 {
    alignas(16) Reg128 gpr[32]{};
    Reg128 hi{};
    Reg128 lo{};
    u32 cop0[32]{};
    cop0::TlbEntry tlb[cop0::kTlbEntries]{};
    u32 performance[2]{};
    u32 performanceControl = 0;
    u32 sa = 0;
    u32 pc = 0;
    // CPCOND0, which bc0f reads. The DMA controller drives it; nothing does yet.
    bool cop0condition = false;

    // A 32-bit result sign-extends through bit 63 and leaves the upper half
    // alone, so a later sq stores whatever was already there.
    void write32(u32 reg, u32 value) {
        if (reg != 0) {
            gpr[reg].lo = static_cast<u64>(static_cast<s64>(static_cast<s32>(value)));
        }
    }

    void write64(u32 reg, u64 value) {
        if (reg != 0) {
            gpr[reg].lo = value;
        }
    }

    void write128(u32 reg, Reg128 value) {
        if (reg != 0) {
            gpr[reg] = value;
        }
    }
};

// One instruction per cycle; any other event stays still rather than reporting
// a number this runtime cannot know.
//
// Only the question is inline: the answer costs seventy bytes a block.
void addPerformanceEvents(R5900& cpu, u32 instructions);

inline void advanceCycles(R5900& cpu, u32 instructions) {
    const u32 before = cpu.cop0[cop0::kCount];
    cpu.cop0[cop0::kCount] = before + instructions;
    if (cpu.cop0[cop0::kCompare] - before - 1u < instructions) {
        cpu.cop0[cop0::kCause] |= cop0::kCounterInterrupt;
    }
    if (cpu.performanceControl != 0) {
        addPerformanceEvents(cpu, instructions);
    }
}

}  // namespace uee
