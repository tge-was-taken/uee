#include "gs.h"

namespace uee {
namespace {

constexpr u32 kCsrRegister = 0x12001000;
constexpr u32 kImrRegister = 0x12001010;

constexpr u32 kSignal = 1u << 0;
constexpr u32 kFinish = 1u << 1;
constexpr u32 kVsint = 1u << 3;
constexpr u32 kField = 1u << 13;

// Write one to clear, so a guest can acknowledge one bit without losing the rest.
constexpr u32 kInterruptBits = 0x1Fu;

}  // namespace

std::optional<u32> Gs::read(u32 address) const {
    switch (address) {
        case kCsrRegister: return mCsr;
        case kImrRegister: return mImr;
        default: return std::nullopt;
    }
}

bool Gs::write(u32 address, u32 value) {
    switch (address) {
        case kCsrRegister:
            mCsr &= ~(value & kInterruptBits);
            return true;
        case kImrRegister:
            mImr = value;
            return true;
        default:
            return false;
    }
}

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
