#include "interrupts.h"

#include <cstddef>

namespace uee {
namespace {

constexpr u32 kDmacMaskShift = 16;

}  // namespace

static_assert(offsetof(IntrHandler, link) == 0,
              "the queue links must start the handler so a Link* names one");

// KERNEL: InitIntrHandlers @ 0x80002050
Interrupts::Interrupts() {
    for (HandlerChain& chain : mIntcChains) {
        initQueue(chain.head);
    }
    for (HandlerChain& chain : mDmacChains) {
        initQueue(chain.head);
    }
    initQueue(mFreeList);
    // Handler ids start at 1; slot 0 is never allocated so that 0 can mean
    // "insert at the tail" in the add calls.
    for (usize i = 1; i < kIntrHandlers; ++i) {
        mPool[i].state = handler_state::kFree;
        insertHead(mFreeList, mPool[i].link);
    }
}

usize Interrupts::indexOf(const Link* node) const {
    return static_cast<usize>(reinterpret_cast<const IntrHandler*>(node) - mPool.data());
}

std::optional<u32> Interrupts::read(u32 address) const {
    switch (address) {
        case kIntcStat: return mIntcStat;
        case kIntcMask: return mIntcMask;
        case kDCtrl: return mDControl;
        case kDStat: return mDStat;
        case kDPcr: return mDPcr;
        default: return std::nullopt;
    }
}

// INTC_MASK and the mask half of D_STAT are write-one-to-toggle; the status
// halves are write-one-to-clear.
// KERNEL: _EnableIntc and _EnableDmac @ 0x800008c0
bool Interrupts::write(u32 address, u32 value) {
    switch (address) {
        case kIntcStat: mIntcStat &= ~value; return true;
        case kIntcMask: mIntcMask ^= value; return true;
        case kDCtrl: mDControl = value; return true;
        case kDStat:
            mDStat &= ~(value & 0xFFFFu);
            mDStat ^= value & 0xFFFF0000u;
            return true;
        case kDPcr: mDPcr = value; return true;
        default: return false;
    }
}

void Interrupts::raiseIntc(u32 cause) {
    mIntcStat |= 1u << cause;
}

void Interrupts::raiseDmac(u32 channel) {
    mDStat |= 1u << channel;
}

std::optional<u32> Interrupts::pendingIntc() const {
    const u32 active = mIntcStat & mIntcMask;
    for (u32 cause = 0; cause < kIntcCauses; ++cause) {
        if ((active & (1u << cause)) != 0) {
            return cause;
        }
    }
    return std::nullopt;
}

std::optional<u32> Interrupts::pendingDmac() const {
    const u32 active = (mDStat & 0xFFFFu) & (mDStat >> kDmacMaskShift);
    for (u32 channel = 0; channel < kDmacChannels; ++channel) {
        if ((active & (1u << channel)) != 0) {
            return channel;
        }
    }
    return std::nullopt;
}

void Interrupts::acknowledgeIntc(u32 cause) {
    mIntcStat &= ~(1u << cause);
}

void Interrupts::acknowledgeDmac(u32 channel) {
    mDStat &= ~(1u << channel);
}

bool Interrupts::conditionZero() const {
    const u32 selected = mDPcr & 0x3FFu;
    return selected == 0 || (mDStat & selected) == selected;
}

// KERNEL: AllocIntrHandler @ 0x800015a0
s32 Interrupts::allocate() {
    Link* node = removeTail(mFreeList);
    if (node == nullptr) {
        return -1;
    }
    ++mHandlerCount;
    return static_cast<s32>(indexOf(node));
}

// KERNEL: AddIntcHandlerInternal @ 0x800018b0
s32 Interrupts::add(HandlerChain& chain, u32 func, s32 next, u32 argument, s32 state) {
    const s32 id = allocate();
    if (id < 0) {
        return -1;
    }
    IntrHandler& entry = mPool[static_cast<usize>(id)];
    entry.func = func;
    entry.argument = argument;
    entry.state = state;

    if (next == -1) {
        insertHead(chain.head, entry.link);
    } else if (next == 0) {
        insertHead(*chain.head.prev, entry.link);
    } else {
        if (next < 1 || static_cast<usize>(next) >= kIntrHandlers ||
            mPool[static_cast<usize>(next)].state == handler_state::kFree) {
            return -1;
        }
        insertHead(mPool[static_cast<usize>(next)].link, entry.link);
    }
    ++chain.count;
    return id;
}

// KERNEL: AddIntcHandler @ 0x80001a30
s32 Interrupts::addIntcHandler(u32 cause, u32 func, s32 next, u32 argument, s32 state) {
    if (cause >= kIntcCauses) {
        return -1;
    }
    // SBUS belongs to AddSbusIntcHandler unless the caller is the kernel.
    if (cause == intc::kSbus && state != handler_state::kKernel) {
        return -1;
    }
    return add(mIntcChains[cause], func, next, argument, state);
}

// KERNEL: AddDmacHandler @ 0x80001c78
s32 Interrupts::addDmacHandler(u32 channel, u32 func, s32 next, u32 argument, s32 state) {
    if (channel >= kDmacChannels) {
        return -1;
    }
    return add(mDmacChains[channel], func, next, argument, state);
}

// KERNEL: RemoveIntcHandler @ 0x80001a70
s32 Interrupts::remove(HandlerChain& chain, s32 id) {
    if (id < 1 || static_cast<usize>(id) >= kIntrHandlers) {
        return -1;
    }
    IntrHandler& entry = mPool[static_cast<usize>(id)];
    if (entry.state == handler_state::kFree) {
        return -1;
    }
    entry.state = handler_state::kFree;
    entry.func = 0;
    unlink(entry.link);
    insertHead(mFreeList, entry.link);
    --mHandlerCount;
    --chain.count;
    return chain.count;
}

// KERNEL: RemoveIntcHandler @ 0x80001a70
s32 Interrupts::removeIntcHandler(u32 cause, s32 id) {
    if (cause >= kIntcCauses) {
        return -1;
    }
    return remove(mIntcChains[cause], id);
}

// KERNEL: RemoveDmacHandler @ 0x80001cb8
s32 Interrupts::removeDmacHandler(u32 channel, s32 id) {
    if (channel >= kDmacChannels) {
        return -1;
    }
    return remove(mDmacChains[channel], id);
}

// KERNEL: EnableIntcHandler @ 0x80001fa0
s32 Interrupts::setHandlerState(s32 id, s32 state) {
    if (id < 1 || static_cast<usize>(id) >= kIntrHandlers) {
        return -1;
    }
    mPool[static_cast<usize>(id)].state = state;
    return id;
}

// KERNEL: DispatchIntcHandlers @ 0x80001630
std::vector<const IntrHandler*> Interrupts::walk(const HandlerChain& chain) const {
    std::vector<const IntrHandler*> handlers;
    const Link* node = chain.head.prev;
    for (s32 i = 0; i < chain.count && node != &chain.head; ++i) {
        handlers.push_back(reinterpret_cast<const IntrHandler*>(node));
        node = node->prev;
    }
    return handlers;
}

std::vector<const IntrHandler*> Interrupts::intcChain(u32 cause) const {
    return cause < kIntcCauses ? walk(mIntcChains[cause]) : std::vector<const IntrHandler*>{};
}

std::vector<const IntrHandler*> Interrupts::dmacChain(u32 channel) const {
    return channel < kDmacChannels ? walk(mDmacChains[channel])
                                   : std::vector<const IntrHandler*>{};
}

}  // namespace uee
