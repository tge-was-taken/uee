#pragma once

#include "types.h"

namespace uee {

struct EeContext;

// Redirects guest functions to host code. The run loop asks isHooked about
// every address it is about to execute, so that check is on the hot path;
// enter runs only for an address that answered yes.
class CallHooks {
public:
    virtual ~CallHooks() = default;

    virtual bool isHooked(u32 pc) const = 0;

    // Returns true when the guest body must not run, in which case the pc has
    // already been moved to wherever execution resumes.
    virtual bool enter(EeContext& ee) = 0;
};

}  // namespace uee
