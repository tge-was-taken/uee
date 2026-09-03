#pragma once

#include "mmio.h"
#include "types.h"

#include <optional>

namespace uee {

constexpr u32 kCsrRegister = 0x12001000;
constexpr u32 kImrRegister = 0x12001010;

// Write one to clear, so a guest can acknowledge one bit without losing the rest.
constexpr u64 kGsInterruptBits = 0x1Full;

// The privileged registers are 64 bits wide. A guest reaches them whole, and
// the word accessors are here so the translator can fold a polled read.
class Gs {
public:
    std::optional<u64> readDouble(u32 address) const {
        switch (address) {
            case kCsrRegister: return mCsr;
            case kImrRegister: return mImr;
            default: return std::nullopt;
        }
    }

    bool writeDouble(u32 address, u64 value) {
        switch (address) {
            case kCsrRegister:
                mCsr &= ~(value & kGsInterruptBits);
                return true;
            case kImrRegister:
                mImr = value;
                return true;
            default:
                return false;
        }
    }

    std::optional<u32> read(u32 address) const {
        if (const std::optional<u64> whole = readDouble(address & ~7u)) {
            return static_cast<u32>((address & 4) != 0 ? *whole >> 32 : *whole);
        }
        return std::nullopt;
    }

    bool write(u32 address, u32 value) {
        const std::optional<u64> whole = readDouble(address & ~7u);
        if (!whole) {
            return false;
        }
        const u64 merged = (address & 4) != 0
                               ? (*whole & 0xFFFFFFFFull) | (static_cast<u64>(value) << 32)
                               : (*whole & ~0xFFFFFFFFull) | value;
        return writeDouble(address & ~7u, merged);
    }

    void raiseSignal();
    void raiseFinish();

    void advanceScanline();
    void startVblank();

    u64 csr() const { return mCsr; }

private:
    u64 mCsr = 0;
    u64 mImr = 0;
    bool mDrawPending = false;
};

}  // namespace uee
