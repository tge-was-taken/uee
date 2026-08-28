#pragma once

#include "types.h"

#include <string>

namespace uee {

enum class Status {
    Executed,
    Unimplemented,  // a real encoding this interpreter does not model yet
    Undecodable,    // no source names this encoding
};

enum class StopReason {
    Exited,
    Returned,
    ReachedLimit,
    Unimplemented,
    Undecodable,
    UnknownSyscall,
    UnmappedAccess,
    Deadlocked,
    WentIdle,
};

struct Stop {
    StopReason reason = StopReason::ReachedLimit;
    u32 pc = 0;
    std::string detail;
};

std::string describe(const Stop& stop);

}  // namespace uee
