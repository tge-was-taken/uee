#pragma once

#include "queue.h"
#include "types.h"

#include <array>
#include <bit>
#include <optional>
#include <vector>

namespace uee {

constexpr u32 kIntcBase = 0x1000F000;
constexpr u32 kIntcStat = kIntcBase + 0x00;
constexpr u32 kIntcMask = kIntcBase + 0x10;

constexpr u32 kDmacBase = 0x1000E000;
constexpr u32 kDCtrl = kDmacBase + 0x00;
constexpr u32 kDStat = kDmacBase + 0x10;
constexpr u32 kDPcr = kDmacBase + 0x20;
constexpr u32 kDRingSize = kDmacBase + 0x40;
constexpr u32 kDRingBase = kDmacBase + 0x50;
constexpr u32 kDStallAddress = kDmacBase + 0x60;
// Every address the controller holds is quadword aligned and inside memory.
constexpr u32 kDmacAddressMask = 0x7FFFFFF0;
// The mask half of the status register, from writing all ones and reading back:
// four of the sixteen places have no channel behind them.
constexpr u32 kDmacMaskBits = 0x63BF0000;
// The channels the priority register can name, and the bits the control
// register keeps, from writing all ones and reading back.
constexpr u32 kDmacPriorityMask = 0x83FF03FF;
constexpr u32 kDmacControlMask = 0x000007FF;
constexpr u32 kMfifoDrainMask = 0xC;

// The lowest cause raised, which is the one the guest is dispatched for.
inline std::optional<u32> lowestSet(u32 active) {
    return active == 0 ? std::nullopt : std::optional(std::countr_zero(active));
}

constexpr u32 kDmacMaskShift = 16;
constexpr usize kIntcCauses = 15;
constexpr usize kDmacChannels = 16;
constexpr usize kIntrHandlers = 129;

namespace intc {
constexpr u32 kGs = 0;
constexpr u32 kSbus = 1;
constexpr u32 kVblankStart = 2;
constexpr u32 kVblankEnd = 3;
constexpr u32 kTimer3 = 12;
}  // namespace intc

namespace dmac {
constexpr u32 kSif0 = 5;
constexpr u32 kSif1 = 6;
}  // namespace dmac

namespace handler_state {
constexpr s32 kKernel = 0;
constexpr s32 kUser = 2;
constexpr s32 kFree = 3;
}  // namespace handler_state

// Mirrors EEIntrHandler.
struct IntrHandler {
    Link link;
    u32 func = 0;
    u32 gp = 0;
    u32 argument = 0;
    s32 state = handler_state::kFree;
};

struct HandlerChain {
    Link head;
    s32 count = 0;
};

// INTC_STAT/MASK and the DMAC registers, plus the handler chains the kernel
// dispatches through them.
class Interrupts {
public:
    Interrupts();

    std::optional<u32> read(u32 address) const;
    bool write(u32 address, u32 value);

    void raiseIntc(u32 cause);
    void raiseDmac(u32 channel);
    u32 readDStat() const { return mDStat; }

    // The ring a transfer wraps inside while the controller drains one, and
    // which channel it drains to. Zero means no ring is in use.
    u32 ringSize() const { return (mDControl & kMfifoDrainMask) != 0 ? mDRingSize : 0; }
    u32 ringBase() const { return mDRingBase; }

    // The display keeps asserting both blanking causes and nothing the boot rom
    // left behind acknowledges them, so a program finds them standing and finds
    // them again after clearing them.
    static constexpr u32 kBlanking = (1u << 2) | (1u << 3);
    // The two the rom leaves unmasked besides the one this kernel adds.
    static constexpr u32 kBootIntcMask = (1u << 1) | (1u << 12);

    // The lowest pending cause that is not masked, if any.
    // Asked every service interval, so they answer where they are called from.
    std::optional<u32> pendingIntc() const {
        return lowestSet(mIntcStat & mIntcMask & ((1u << kIntcCauses) - 1));
    }
    std::optional<u32> pendingDmac() const {
        return lowestSet((mDStat & 0xFFFFu) & (mDStat >> kDmacMaskShift));
    }
    void acknowledgeIntc(u32 cause);
    void acknowledgeDmac(u32 channel);

    s32 addIntcHandler(u32 cause, u32 func, s32 next, u32 argument, s32 state);
    s32 addDmacHandler(u32 channel, u32 func, s32 next, u32 argument, s32 state);
    s32 removeIntcHandler(u32 cause, s32 id);
    s32 removeDmacHandler(u32 channel, s32 id);
    s32 setHandlerState(s32 id, s32 state);

    // Handlers for a cause, oldest first, as the kernel dispatches them.
    std::vector<const IntrHandler*> intcChain(u32 cause) const;
    std::vector<const IntrHandler*> dmacChain(u32 channel) const;

    s32 handlerCount() const { return mHandlerCount; }
    // The DMAC drives CPCOND0 from the channels D_PCR selects.
    bool conditionZero() const;

private:
    s32 allocate();
    s32 add(HandlerChain& chain, u32 func, s32 next, u32 argument, s32 state);
    s32 remove(HandlerChain& chain, s32 id);
    std::vector<const IntrHandler*> walk(const HandlerChain& chain) const;
    usize indexOf(const Link* node) const;

    std::array<IntrHandler, kIntrHandlers> mPool;
    std::array<HandlerChain, kIntcCauses> mIntcChains;
    std::array<HandlerChain, kDmacChannels> mDmacChains;
    Link mFreeList;

    u32 mIntcStat = kBlanking;
    u32 mIntcMask = kBootIntcMask;
    // The boot rom leaves the controller enabled.
    u32 mDControl = 1;
    // What the boot rom leaves behind: the two SIF channels it unmasked and the
    // transfer it took to hand the program over, still flagged.
    u32 mDStat = (1u << (7 + kDmacMaskShift)) | (1u << 6);
    u32 mDPcr = 0;
    u32 mDRingSize = 0;
    u32 mDRingBase = 0;
    u32 mDStallAddress = 0;
    s32 mHandlerCount = 0;
};

}  // namespace uee
