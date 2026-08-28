#pragma once

#include "ee_context.h"
#include "fpu.h"
#include "r5900.h"
#include "types.h"

namespace uee {

// KERNEL: EEContext, 640 bytes. COP0 is not in it, so Status stays with the
// processor and belongs to the exception return, and neither is the program
// counter: that lives in EEThreadCB.resume_pc.
struct ThreadContext {
    Reg128 gpr[32]{};
    Reg128 hi{};
    Reg128 lo{};
    u32 sa = 0;
    Fpu fpu;

    void saveFrom(const EeContext& context) {
        for (usize i = 0; i < 32; ++i) {
            gpr[i] = context.cpu.gpr[i];
        }
        hi = context.cpu.hi;
        lo = context.cpu.lo;
        sa = context.cpu.sa;
        fpu = context.fpu;
    }

    void loadInto(EeContext& context) const {
        for (usize i = 0; i < 32; ++i) {
            context.cpu.gpr[i] = gpr[i];
        }
        context.cpu.hi = hi;
        context.cpu.lo = lo;
        context.cpu.sa = sa;
        context.fpu = fpu;
    }
};

}  // namespace uee
