#pragma once

#include "types.h"

namespace uee {

struct EeContext;

class Hook {
public:
    virtual ~Hook() = default;

    virtual bool isHooked(u32 pc) const = 0;

    // True when a frontend answered and left the pc where execution resumes.
    // isHooked is on the hot path; this runs only for an address it accepted.
    virtual bool enter(EeContext& ee) = 0;
};

}  // namespace uee
