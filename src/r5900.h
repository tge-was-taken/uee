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

constexpr u32 kStatus = 12;
constexpr u32 kCause = 13;
constexpr u32 kEpc = 14;
constexpr u32 kProcessorId = 15;
// What an R5900 reports: revision 2.2 of implementation 0x2e.
constexpr u32 kR5900Id = 0x00002e20;
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
constexpr u32 kBreakpoint = 9;
}  // namespace cop0

struct R5900 {
    alignas(16) Reg128 gpr[32]{};
    Reg128 hi{};
    Reg128 lo{};
    u32 cop0[32]{};
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

}  // namespace uee
