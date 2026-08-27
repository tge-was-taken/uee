#include "hardware.h"

namespace uee {

std::optional<u32> Hardware::read(u32 address) const {
    if (const std::optional<u32> value = mTimers.read(address)) {
        return value;
    }
    return mInterrupts.read(address);
}

bool Hardware::write(u32 address, u32 value) {
    return mTimers.write(address, value) || mInterrupts.write(address, value);
}

}  // namespace uee
