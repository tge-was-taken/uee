#include "gs.h"

namespace uee {
namespace {

constexpr u32 kSignal = 1u << 0;
constexpr u32 kFinish = 1u << 1;
constexpr u32 kVsint = 1u << 3;
constexpr u32 kField = 1u << 13;

}  // namespace

void Gs::raiseSignal() { mCsr |= kSignal; }

void Gs::raiseFinish() { mDrawPending = true; }

// Nothing here draws, so a scanline stands in for however long the unit would
// have spent on the packet the GIF handed it.
void Gs::advanceScanline() {
    if (mDrawPending) {
        mCsr |= kFinish;
        mDrawPending = false;
    }
}

void Gs::startVblank() { mCsr = (mCsr ^ kField) | kVsint; }

}  // namespace uee
