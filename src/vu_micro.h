#pragma once

#include "types.h"
#include "vu.h"

namespace uee {

class Memory;

class VuMicro {
public:
    VuMicro(Vu& state, Memory& memory, u32 unit) : mState(state), mMemory(memory), mUnit(unit) {}

    u32 run(u32 start);

private:
    u32 instructionBase() const;
    u32 instructionCount() const;
    u32 dataBase() const;
    u32 dataSize() const;

    void executeUpper(u32 code);
    void executeLower(u32 code);
    u32 elementaryFunction(u32 wide, u32 code, u32 vs);
    u32 quotientOf(u32 code, u32 vs, u32 vt);
    u32 rootOf(u32 code, u32 vt);
    u32 inverseRootOf(u32 code, u32 vs, u32 vt);
    u32 sourceLane(u32 code, u32 vf) const;
    void advanceRandom();
    static constexpr u32 kFmacLatency = 4;
    u32 mCycle = 0;
    u32 mWriteReady[32] = {};

    u32 operandsReadyAt(u32 upper, u32 lower) const;
    void stallUntil(u32 cycle);
    void issueQuotient(u32 value, u32 latency);
    void advanceQuotients();
    void settleQuotients();
    void loadQuadword(u32 dest, u32 vf, u32 quadword);
    void storeQuadword(u32 dest, u32 vf, u32 quadword);
    void branchTo(u32 target);

    Vu& mState;
    Memory& mMemory;
    u32 mUnit;

    u32 mProgramCounter = 0;
    u32 mBranchTarget = 0;
    bool mBranching = false;
    bool mEnding = false;
    // A clip result reaches the register a program can read four instructions
    // after the one that worked it out.
    static constexpr u32 kClipDelay = 4;
    u32 mClipDelayLine[kClipDelay] = {};
    // There is one divider, and it takes several instructions to land a result
    // in Q.
    static constexpr u32 kQuotientLatency = 7;
    static constexpr u32 kRootLatency = 13;
    u32 mPendingQuotient = 0;
    u32 mQuotientDelay = 0;
};

}  // namespace uee
