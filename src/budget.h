#pragma once

#include "types.h"

namespace uee {

// How far a run has got and how far it may go. A plain struct because emitted
// code reads it at every block boundary.
struct Budget {
    // How often the clock and the interrupt controller are serviced. A guest
    // cannot see either directly, so this need not be every instruction.
    static constexpr u64 kServiceInterval = 1024;

    // Guest instructions run so far.
    u64 executed = 0;

    // How many of those the performance counters have been told about.
    u64 counted = 0;

    // The next `executed` at which to service.
    u64 nextService = kServiceInterval;

    // Set where the clock is serviced, which is the only place `limit` is read.
    bool stopped = false;

    // Instructions, not wall clock, so a measurement covers the same guest work
    // every time.
    u64 limit = ~u64{0};
};

}  // namespace uee
