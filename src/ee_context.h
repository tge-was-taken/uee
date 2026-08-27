#pragma once

#include "fpu.h"
#include "r5900.h"
#include "vu0.h"

namespace uee {

struct EeContext {
    R5900 cpu;
    Fpu fpu;
    Vu0 vu0;
};

}  // namespace uee
