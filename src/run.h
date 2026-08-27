#pragma once

#include "interpreter.h"
#include "kernel.h"
#include "memory.h"
#include "translate.h"
#include "types.h"

#include <string>

namespace uee {

enum class StopReason {
    Exited,
    Returned,
    ReachedLimit,
    Unimplemented,
    Undecodable,
    UnknownSyscall,
    UnmappedAccess,
    Deadlocked,
};

struct Stop {
    StopReason reason = StopReason::ReachedLimit;
    u32 pc = 0;
    std::string detail;
};

std::string describe(const Stop& stop);

Stop run(Machine& machine, u64 instructionLimit);

}  // namespace uee
