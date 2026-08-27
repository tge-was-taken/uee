#pragma once

#include "interrupts.h"
#include "timers.h"
#include "types.h"

#include <optional>

namespace uee {

// The registers in the 0x10000000 window that this runtime models. Everything
// else in the window stays plain memory.
class Hardware {
public:
    std::optional<u32> read(u32 address) const;
    bool write(u32 address, u32 value);

    Timers& timers() { return mTimers; }
    Interrupts& interrupts() { return mInterrupts; }

private:
    Timers mTimers;
    Interrupts mInterrupts;
};

}  // namespace uee
