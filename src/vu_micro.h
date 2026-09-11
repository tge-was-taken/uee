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

    struct Result {
        u32 counter;
        bool ended;
    };

    Result run(u32 start);

    void begin(u32 start);
    // Runs one instruction pair; true once the program has ended.
    bool step();
    u32 getProgramCounter() const { return mProgramCounter; }
    u32 getCycle() const { return mCycle; }

private:
    u32 getInstructionBase() const;
    u32 getInstructionCount() const;
    u32 getDataBase() const;
    u32 getDataSize() const;

    void executeUpper(u32 code);
    void executeLower(u32 code);
    void executeLowerBeside(u32 upper, u32 lower, const Reg128& upperBefore);
    u32 computeFunction(u32 wide, u32 code, u32 vs);
    static u32 getFunctionLatency(u32 wide);
    u32 computeQuotient(u32 code, u32 vs, u32 vt);
    u32 computeRoot(u32 code, u32 vt);
    u32 computeInverseRoot(u32 code, u32 vs, u32 vt);
    u32 takeQuotient(Vu::Quotient q);
    u32 readFtLane(u32 code, u32 vf) const;
    u32 readFsLane(u32 code, u32 vf) const;
    void advanceRandom();

    u32 getLaneReadyCycle(u32 vf, u32 lanes) const;
    u32 getUpperReadyCycle(u32 code) const;
    u32 getLowerReadyCycle(u32 code) const;
    u32 getOperandsReadyCycle(u32 upper, u32 lower) const;
    void markWritten(u32 upper, u32 lower);
    void stallUntil(u32 cycle);
    void beginCycle();
    void endCycle();
    void syncPipelines();

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

    static constexpr u32 kFmacLatency = 4;
    static constexpr u32 kIntegerLoadLatency = 4;
    u32 mCycle = 0;
    u32 mLaneReady[32][4] = {};
    u32 mIntegerReady[16] = {};

    // The flags reach the registers a program reads when the op that set them
    // leaves the pipeline. P3FES tests a flag four instructions after setting it.
    struct FlagSet {
        u32 status;
        u32 mac;
        u32 clip;
    };
    static constexpr u32 kFlagDelay = 4;
    FlagSet mFlagDelayLine[kFlagDelay] = {};
    FlagSet mReadableFlags = {};

    static constexpr u32 kQuotientLatency = 7;
    static constexpr u32 kRootLatency = 13;

    // A result the unit hands over some cycles after the op that asked for it;
    // reading the register before then sees what it held.
    struct DelayedResult {
        u32 value = 0;
        u32 delay = 0;

        void issue(u32 next, u32 latency) {
            value = next;
            delay = latency;
        }
        void advance(u32& target) {
            if (delay != 0 && --delay == 0) {
                target = value;
            }
        }
        void sync(u32& target) {
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
