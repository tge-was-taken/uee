#pragma once

#include "interpreter.h"
#include "kernel.h"
#include "memory.h"
#include "stop.h"
#include "ee_translate.h"
#include "types.h"

#include <chrono>
#include <optional>

namespace uee {

// Ends when the guest exits, when the host asks it to stop, or when the limit
// has passed. No limit means the first two only.
Stop run(Machine& machine, std::optional<std::chrono::milliseconds> limit = {});

}  // namespace uee
