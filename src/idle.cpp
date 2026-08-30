#include "idle.h"

#include <algorithm>
#include <utility>

namespace uee {
namespace {

constexpr u32 kRegionShift = 10;
constexpr u32 kSamples = 256;

}  // namespace

Idle::Idle(std::string processor, const Symbols& symbols)
    : mSymbols(symbols), mProcessor(std::move(processor)) {}

bool Idle::sample(u32 pc) {
    if (mLog.tracing()) {
        mLog.trace("{} at {}", mProcessor, mSymbols.describe(pc));
    }

    const u32 region = pc >> kRegionShift;
    const auto end = mRegions.begin() + static_cast<isize>(mRegionCount);
    if (std::find(mRegions.begin(), end, region) == end) {
        if (mRegionCount == kRegions) {
            mRegionCount = 0;
            mSamples = 0;
            mReported = false;
            return false;
        }
        mSeen[mRegionCount] = pc;
        mRegions[mRegionCount++] = region;
    }

    if (++mSamples < kSamples || mReported) {
        return false;
    }
    mReported = true;
    report();
    return true;
}

void Idle::report() const {
    std::string places;
    for (usize i = 0; i < mRegionCount; ++i) {
        places += (i == 0 ? "" : ", ") + mSymbols.describe(mSeen[i]);
    }
    mLog.warn("{} has spent {} samples in {}", mProcessor, mSamples, places);
}

}  // namespace uee
