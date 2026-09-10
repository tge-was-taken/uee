#pragma once

#include "types.h"

#include <optional>

namespace uee {

namespace sbus {

constexpr u32 kMainToSubCommand = 0x1000F200;
constexpr u32 kSubToMainCommand = 0x1000F210;
constexpr u32 kMainToSubFlag = 0x1000F220;
constexpr u32 kSubToMainFlag = 0x1000F230;
constexpr u32 kControl = 0x1000F240;
// The block answers a second time eight registers up.
constexpr u32 kMirror = 0x80;
constexpr u32 kMirrorBase = kMainToSubCommand + kMirror;
constexpr u32 kMirrorEnd = kControl + kMirror;

// What the boot rom leaves: the interface up, and a status field in the
// control register that the first read takes away.
constexpr u32 kBootControl = 0x00F00142;
constexpr u32 kControlKept = 0x0000FFFF;

constexpr u32 kSifInitialised = 0x10000;
constexpr u32 kCommandInitialised = 0x20000;
constexpr u32 kBootFinished = 0x40000;

}  // namespace sbus

// Each side sets bits only in its own flag word and clears bits only in the
// other's, so neither can lose a flag the other raised.
class Sbus {
public:
    std::optional<u32> read(u32 address);
    bool write(u32 address, u32 value);

    void raiseSubToMainFlag(u32 bits) { mSubToMainFlag |= bits; }
    void writeSubToMainCommand(u32 value) { mSubToMainCommand = value; }
    u32 subToMainFlag() const { return mSubToMainFlag; }
    u32 mainToSubCommand() const { return mMainToSubCommand; }

private:
    u32 mMainToSubCommand = 0;
    u32 mSubToMainCommand = 0;
    u32 mMainToSubFlag = sbus::kSifInitialised;
    u32 mSubToMainFlag = 0;
    u32 mControl = sbus::kBootControl;
};

}  // namespace uee
