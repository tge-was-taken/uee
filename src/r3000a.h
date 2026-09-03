#pragma once

#include "types.h"

namespace uee {

namespace r3000 {
constexpr u32 kStatus = 12;
constexpr u32 kCause = 13;
constexpr u32 kEpc = 14;
constexpr u32 kCauseCodeShift = 2;
constexpr u32 kCauseCodeMask = 0x1Fu << kCauseCodeShift;
constexpr u32 kBreakpoint = 9;
// The R3000A has no separate vector for a breakpoint; everything but a reset
// arrives here, or at the boot copy while the status says so.
constexpr u32 kExceptionVector = 0x80000080;
constexpr u32 kBootExceptionVector = 0xBFC00180;
constexpr u32 kBootExceptionVectors = 1u << 22;
}  // namespace r3000

struct DelayedLoad {
    u32 reg = 0;
    u32 value = 0;
};

struct R3000A {
    u32 gpr[32]{};
    u32 cop0[32]{};
    u32 hi = 0;
    u32 lo = 0;
    u32 pc = 0;

    // There is no load interlock: a load's result waits a slot before it
    // reaches the register file.
    DelayedLoad load;
    DelayedLoad issued;

    void write(u32 reg, u32 value) {
        if (reg == 0) {
            return;
        }
        gpr[reg] = value;
        if (reg == load.reg) {
            load.reg = 0;
        }
    }

    void delayLoad(u32 reg, u32 value) { issued = {reg, value}; }
};

// Must run before every instruction: that is what makes a load readable two
// instructions on, and lets the one in between cancel it.
inline void commitLoad(R3000A& cpu) {
    if (cpu.load.reg != 0) {
        cpu.gpr[cpu.load.reg] = cpu.load.value;
    }
    cpu.load = cpu.issued;
    cpu.issued = {};
}

}  // namespace uee
