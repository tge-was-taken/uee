#pragma once

#include "interpreter.h"
#include "kernel.h"
#include "memory.h"
#include "stop.h"
#include "translate.h"
#include "types.h"

namespace uee {

Stop run(Machine& machine, u64 instructionLimit);

}  // namespace uee
