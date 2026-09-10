#pragma once

#include "hook.h"

namespace sky2 {

// The Hooks:: registry as the run loop sees it. Ask for it after the guest RAM
// overlay is set: registering a hook plants a stub in RAM.
uee::Hook& getHook();

}  // namespace sky2
