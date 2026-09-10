#include "sbus.h"

namespace uee {

std::optional<u32> Sbus::read(u32 address) {
    if (address >= sbus::kMirrorBase && address <= sbus::kMirrorEnd) {
        address -= sbus::kMirror;
    }
    switch (address) {
        case sbus::kMainToSubCommand: return mMainToSubCommand;
        case sbus::kSubToMainCommand: return mSubToMainCommand;
        case sbus::kMainToSubFlag: return mMainToSubFlag;
        case sbus::kSubToMainFlag: return mSubToMainFlag;
        case sbus::kControl: {
            const u32 seen = mControl;
            mControl &= sbus::kControlKept;
            return seen;
        }
        default: return std::nullopt;
    }
}

bool Sbus::write(u32 address, u32 value) {
    switch (address) {
        case sbus::kMainToSubCommand: mMainToSubCommand = value; return true;
        case sbus::kMainToSubFlag: mMainToSubFlag |= value; return true;
        case sbus::kSubToMainFlag: mSubToMainFlag &= ~value; return true;
        default: return false;
    }
}

}  // namespace uee
