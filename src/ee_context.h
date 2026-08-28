#pragma once

#include "fpu.h"
#include "r5900.h"
#include "vu.h"

namespace uee {

struct EeContext {
    R5900 cpu;
    Fpu fpu;
    Vu vu0;
};

}  // namespace uee
