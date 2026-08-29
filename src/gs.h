#pragma once

#include "types.h"

#include <optional>

namespace uee {

class Gs {
public:
    std::optional<u32> read(u32 address) const;
    bool write(u32 address, u32 value);

    void raiseSignal();
    void raiseFinish();

    void advanceScanline();
    void startVblank();

    u64 csr() const { return mCsr; }

private:
    u32 mCsr = 0;
    u32 mImr = 0;
    bool mDrawPending = false;
};

}  // namespace uee
