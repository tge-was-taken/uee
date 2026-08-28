#pragma once

#include "types.h"

#include <array>
#include <optional>

namespace uee {

class Gif;
class Vif;

class Memory;
class Interrupts;

namespace dmac {

constexpr usize kChannels = 10;
constexpr usize kReturnDepth = 2;
constexpr u32 kEnableRead = 0x1000F520;
constexpr u32 kEnableWrite = 0x1000F590;
constexpr u32 kSuspended = 0x10000;

}  // namespace dmac

// The ten DMA channels. A transfer runs to completion the moment the guest sets
// the start bit, so a channel is never seen busy.
class Dmac {
public:
    void attach(Memory& memory, Interrupts& interrupts, Gif& gif, Vif& vif0, Vif& vif1);

    std::optional<u32> read(u32 address) const;
    bool write(u32 address, u32 value);

private:
    struct Channel {
        u32 control = 0;
        u32 address = 0;
        u32 quadwords = 0;
        u32 tagAddress = 0;
        u32 returnAddress[dmac::kReturnDepth] = {};
        usize returnDepth = 0;
        u32 scratchpadAddress = 0;
    };

    void start(usize index);
    void interleave(usize index);
    void transfer(usize index, u32 address, u32 quadwords);
    void followChain(usize index);

    std::array<Channel, dmac::kChannels> mChannels;
    u32 mEnable = 0;
    u32 mSkipQuadword = 0;
    Gif* mGif = nullptr;
    Vif* mVif[2] = {nullptr, nullptr};
    Memory* mMemory = nullptr;
    Interrupts* mInterrupts = nullptr;
};

}  // namespace uee
