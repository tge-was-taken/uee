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
    TimedOut,
    Unimplemented,
    Undecodable,
    UnknownSyscall,
    UnmappedAccess,
    Deadlocked,
    WentIdle,
    Stopped,
};

struct Stop {
    StopReason reason = StopReason::ReachedLimit;
    u32 pc = 0;
    std::string detail;
};

std::string describe(const Stop& stop);

// Asked between batches of instructions whether the guest should carry on, for
// a host that ends the run from another thread.
class StopRequest {
public:
    virtual ~StopRequest() = default;

    virtual bool isStopRequested() = 0;
};

}  // namespace uee
