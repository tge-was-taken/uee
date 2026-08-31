#pragma once

#include "log.h"
#include "memory.h"
#include "symbols.h"
#include "types.h"

#include <array>
#include <span>
#include <vector>
#include <string>

namespace uee {

// Tells a guest polling hardware nothing here drives apart from one doing work.
class Idle {
public:
    Idle(std::string processor, const Symbols& symbols, Memory& memory,
         std::span<const u32> watched);

    // True on the sample that counts as idle busy loop
    bool sample(u32 pc);

private:
    // A poll loop calls out to read what it waits on, so it visits a few
    // separate regions rather than one span.
    static constexpr usize kRegions = 8;

    void report() const;

    const Symbols& mSymbols;
    Memory& mMemory;
    // Reported beside the spinning loop.
    std::vector<u32> mWatched;
    LoggerChannel mLog{"idle"};
    std::string mProcessor;
    std::array<u32, kRegions> mRegions{};
    // A pc sampled in each region names it better than where the region starts.
    std::array<u32, kRegions> mSeen{};
    usize mRegionCount = 0;
    u32 mSamples = 0;
    bool mReported = false;
};

}  // namespace uee
