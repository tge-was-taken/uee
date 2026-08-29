#pragma once

#include "dmac.h"
#include "gif.h"
#include "gs.h"
#include "interrupts.h"
#include "r5900.h"
#include "sbus.h"
#include "timers.h"
#include "vif.h"
#include "types.h"

#include <optional>

namespace uee {

// The registers in the 0x10000000 window that this runtime models. Everything
// else in the window stays plain memory.
class Mmio {
public:
    void attach(Memory& memory, R5900& cpu) {
        mCpu = &cpu;
        mGif.attach(memory, mGs);
        mVif[0].attach(memory, mGif, 0);
        mVif[1].attach(memory, mGif, 1);
        mDmac.attach(memory, mInterrupts, mGif, mVif[0], mVif[1]);
        refreshCondition();
    }

    // CPCOND0 is an output of the DMAC that the CPU branches on. It is restated
    // wherever a register that feeds it, or the register file that holds it,
    // can have moved.
    void refreshCondition() {
        if (mCpu != nullptr) {
            mCpu->cop0condition = mInterrupts.conditionZero();
        }
    }

    std::optional<u32> read(u32 address) const;
    bool write(u32 address, u32 value);

    Timers& timers() { return mTimers; }
    Interrupts& interrupts() { return mInterrupts; }
    Sbus& sbus() { return mSbus; }
    Dmac& dmac() { return mDmac; }
    Gif& gif() { return mGif; }
    Gs& gs() { return mGs; }
    Vif& vif(u32 unit) { return mVif[unit]; }
    const Sbus& sbus() const { return mSbus; }

private:
    Timers mTimers;
    Interrupts mInterrupts;
    Sbus mSbus;
    Gif mGif;
    Gs mGs;
    Vif mVif[2];
    Dmac mDmac;
    R5900* mCpu = nullptr;
};

}  // namespace uee
