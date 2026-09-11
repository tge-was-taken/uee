#pragma once

#include "types.h"
#include "vu.h"

namespace uee {

class Gif;
class Memory;

class VuMicro {
public:
    VuMicro(Vu& state, Memory& memory, u32 unit, Gif* gif)
        : mState(state), mMemory(memory), mGif(gif), mUnit(unit) {}

    struct Outcome {
        u32 counter;
        bool ended;
    };

    Outcome run(u32 start);

    void begin(u32 start);
    // Runs one instruction pair; true once the program has ended.
    bool step();
    u32 getProgramCounter() const { return mProgramCounter; }

private:
    u32 instructionBase() const;
    u32 instructionCount() const;
    u32 dataBase() const;
    u32 dataSize() const;

    void executeUpper(u32 code);
    void executeLower(u32 code);
    void executeLowerBeside(u32 upper, u32 lower, const Reg128& upperBefore);
    u32 elementaryFunction(u32 wide, u32 code, u32 vs);
    static u32 functionLatency(u32 wide);
    u32 quotientOf(u32 code, u32 vs, u32 vt);
    u32 rootOf(u32 code, u32 vt);
    u32 inverseRootOf(u32 code, u32 vs, u32 vt);
    u32 takeQuotient(Vu::Quotient q);
    u32 targetLane(u32 code, u32 vf) const;
    u32 sourceLane(u32 code, u32 vf) const;
    void advanceRandom();
    static constexpr u32 kFmacLatency = 4;
    u32 mCycle = 0;
    u32 mWriteReady[32] = {};

    u32 operandsReadyAt(u32 upper, u32 lower) const;
    void stallUntil(u32 cycle);
    void advancePipelines();
    void settlePipelines();
    void loadQuadword(u32 dest, u32 vf, u32 quadword);
    void storeQuadword(u32 dest, u32 vf, u32 quadword);
    void branchTo(u32 target);
    void kickToGif(u32 quadword);
    u32 readInterfaceRegister(u32 offset) const;

    Vu& mState;
    Memory& mMemory;
    Gif* mGif;
    u32 mUnit;

    u32 mProgramCounter = 0;
    u32 mBranchTarget = 0;
    bool mBranching = false;
    bool mEnding = false;
    u32 mEndDelay = 1;
    // A clip result reaches the register a program can read four instructions
    // after the one that worked it out.
    static constexpr u32 kClipDelay = 4;
    u32 mClipDelayLine[kClipDelay] = {};
    // There is one divider, and it takes several instructions to land a result
    // in Q.
    static constexpr u32 kQuotientLatency = 7;
    static constexpr u32 kRootLatency = 13;

    // A result the unit hands over some instructions after the one that asked
    // for it; reading the register before then sees what it held.
    struct DelayedResult {
        u32 value = 0;
        u32 delay = 0;

        void issue(u32& target, u32 next, u32 latency) {
            settle(target);
            value = next;
            delay = latency;
        }
        void advance(u32& target) {
            if (delay != 0 && --delay == 0) {
                target = value;
            }
        }
        void settle(u32& target) {
            if (delay != 0) {
                target = value;
                delay = 0;
            }
        }
    };

    DelayedResult mQuotient;
    DelayedResult mFunction;
};

// The address a program stopped at is what the next start reads back, so the
// two always move together.
bool runMicroProgram(Vu& state, Memory& memory, Gif* gif, u32 unit, u32 start);

}  // namespace uee
