#pragma once

#include "memory.h"
#include "types.h"

#include <span>

namespace uee {

u32 loadElf(std::span<const u8> image, Memory& memory);

}  // namespace uee
