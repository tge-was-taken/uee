#pragma once

#include "memory.h"
#include "types.h"

#include <span>

namespace uee {

struct Program {
    u32 entry = 0;
    u32 codeStart = 0;
    u32 codeEnd = 0;
    u32 imageStart = 0;
    u32 imageEnd = 0;
};

Program loadElf(std::span<const u8> image, Memory& memory);

}  // namespace uee
