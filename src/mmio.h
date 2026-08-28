#pragma once

#include "dmac.h"
#include "interrupts.h"
#include "sbus.h"
#include "timers.h"
#include "types.h"

#include <optional>

namespace uee {

// The registers in the 0x10000000 window that this runtime models. Everything
// else in the window stays plain memory.
class Mmio {
public:
    void attach(Memory& memory) { mDmac.attach(memory, mInterrupts); }

    std::optional<u32> read(u32 address) const;
    bool write(u32 address, u32 value);

    Timers& timers() { return mTimers; }
    Interrupts& interrupts() { return mInterrupts; }
    Sbus& sbus() { return mSbus; }
    Dmac& dmac() { return mDmac; }
    const Sbus& sbus() const { return mSbus; }

private:
    Timers mTimers;
    Interrupts mInterrupts;
    Sbus mSbus;
    Dmac mDmac;
};

}  // namespace uee
