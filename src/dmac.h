#pragma once

#include "ipu.h"
#include "types.h"

#include <array>
#include <optional>
#include <vector>

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

// A transfer completes the moment the start bit is set, so none is seen busy.
class Dmac {
public:
    void attach(Memory& memory, Interrupts& interrupts, Gif& gif, Vif& vif0, Vif& vif1,
                Ipu& ipu);


    // What the boot rom leaves on each channel: the direction the one way ones
    // are fixed at, and the two the interface was still using.
    void installBootChannels();

    std::optional<u32> read(u32 address);
    bool write(u32 address, u32 value);

private:
    struct Channel {
        u32 control = 0;
        u32 address = 0;
        u32 quadwords = 0;
        u32 tagAddress = 0;
        u32 returnAddress[dmac::kReturnDepth] = {};
        // Where the transfer ended, which the channel writes back when it
        // retires rather than as it runs.
        u32 retiredAddress = 0;
        bool finishing = false;
        usize returnDepth = 0;
        u32 scratchpadAddress = 0;
        // A transfer this runtime performs at once still has to look like it
        // took time, or a guest that polls the start bit never sees one run.
    };

    static bool isScratchpadChannel(usize index);
    static u32 addressMask(usize index);
    static u32 controlMask(usize index);
    static u32 settledMode(usize index, u32 mode);
    static u32 tagAddressMask(usize index);
    void retire(Channel& channel);
    void start(usize index);
    void interleave(usize index);
    void sendTagCodes(usize index, u32 tagAddress);
    u32 transfer(usize index, u32 address, u32 quadwords);
    void followChain(usize index);

    std::array<Channel, dmac::kChannels> mChannels;
    // What the boot rom leaves in the halt register, which no write here clears.
    u32 mEnable = 0x3101;
    u32 mSkipQuadword = 0;

    Gif* mGif = nullptr;
    Ipu* mIpu = nullptr;
    Vif* mVif[2] = {nullptr, nullptr};
    Memory* mMemory = nullptr;
    Interrupts* mInterrupts = nullptr;
};

}  // namespace uee
