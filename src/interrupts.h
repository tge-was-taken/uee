#pragma once

#include "queue.h"
#include "types.h"

#include <array>
#include <optional>
#include <vector>

namespace uee {

constexpr u32 kIntcStat = 0x1000F000;
constexpr u32 kIntcMask = 0x1000F010;
constexpr u32 kDmacBase = 0x1000E000;
constexpr u32 kDCtrl = 0x1000E000;
constexpr u32 kDStat = 0x1000E010;
constexpr u32 kDPcr = 0x1000E020;

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
    u32 globalPointer = 0;
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

    // The lowest pending cause that is not masked, if any.
    std::optional<u32> pendingIntc() const;
    std::optional<u32> pendingDmac() const;
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

    u32 mIntcStat = 0;
    u32 mIntcMask = 0;
    u32 mDControl = 0;
    u32 mDStat = 0;
    u32 mDPcr = 0;
    s32 mHandlerCount = 0;
};

}  // namespace uee
