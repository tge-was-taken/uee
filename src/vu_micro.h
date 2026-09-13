#pragma once

#include "memory.h"
#include "types.h"
#include "vu.h"
#include "vu_micro_ops.h"

#include <span>

namespace uee {

class Gif;
class VuProgramLog;
struct VuProgramRecord;

// A micro program running on one unit: the unit's state and what a run keeps
// in its pipelines. The handlers in vu_micro_handlers.h work on it, whether the
// interpreter decoded the pair or a translated program names them directly.
struct VuMicro {
    VuMicro(Vu& state, Memory& memory, u32 unit, Gif* gif);

    struct Result {
        u32 counter;
        bool ended;
    };

    // The hardware unit runs beside the processor, so a program with no end bit
    // spins there forever while the guest carries on. Running it here has to
    // stop somewhere, and a program that means to end does so long before this.
    static constexpr u32 kRunawayStep = 1'000'000;
    static constexpr u32 kFmacLatency = 4;
    static constexpr u32 kIntegerLoadLatency = 4;
    static constexpr u32 kQuotientLatency = 7;
    static constexpr u32 kRootLatency = 13;

    Result run(u32 start);
    void begin(u32 start);
    // Runs one instruction pair; true once the program has ended.
    bool step();

    u32 getInstructionCount() const { return static_cast<u32>(instructions.size() / 8); }
    u32 getDataBase() const { return unit == 0 ? kVu0DataBase : kVu1DataBase; }
    u32 getDataSize() const { return unit == 0 ? kVu0MemorySize : kVu1MemorySize; }

    // The flags reach the registers a program reads when the op that set them
    // leaves the pipeline. P3FES tests a flag four instructions after setting it.
    struct FlagSet {
        u32 status;
        u32 mac;
        u32 clip;
    };
    static constexpr u32 kFlagDelay = 4;

    // Flags as a pair left them, readable from readyCycle on.
    struct FlagRecord {
        u32 readyCycle;
        FlagSet flags;
    };

    // A result the unit hands over some cycles after the op that asked for it;
    // reading the register before then sees what it held.
    struct DelayedResult {
        u32 value = 0;
        u32 readyCycle = 0;
        bool pending = false;

        u32 getLandingCycle() const { return pending ? readyCycle : ~0u; }
        void land(u32 cycle, u32& target) {
            if (pending && cycle >= readyCycle) {
                target = value;
                pending = false;
            }
        }
        void sync(u32& target) {
            if (pending) {
                target = value;
                pending = false;
            }
        }
    };

    Vu& state;
    Memory& memory;
    Gif* gif;
    u32 unit;
    // Held directly: going through Memory searched its regions on every fetch.
    std::span<u8> instructions;
    std::span<u8> data;
    // Where the run is recorded for translating later. The record is looked up
    // when the interpreter first takes a pair, so a translated run pays nothing.
    VuProgramLog* log = nullptr;
    VuProgramRecord* record = nullptr;
    u32 entry = 0;

    u32 programCounter = 0;
    u32 branchTarget = 0;
    bool branching = false;
    // The branch set by the pair before, which this pair's end hands on.
    bool taken = false;
    u32 takenTarget = 0;
    bool ending = false;
    u32 endDelay = 1;

    u32 cycle = 0;
    u32 laneReady[32][4] = {};
    u32 integerReady[16] = {};

    // Only pairs that change the flags add a record, one cycle apart at least,
    // so the newest one ready is always among the last kFlagDelay.
    FlagRecord flagHistory[kFlagDelay] = {};
    u32 flagNewest = 0;

    DelayedResult quotient;
    DelayedResult function;
    // The earlier of the two results still in flight, checked once a cycle.
    u32 landingCycle = ~0u;
};

// Where a program reads the interface's double buffer pointers.
constexpr u32 getVifTopAddress(u32 unit) { return (unit == 0 ? 0x10003800u : 0x10003C00u) + 0xE0u; }
constexpr u32 getVifItopAddress(u32 unit) {
    return (unit == 0 ? 0x10003800u : 0x10003C00u) + 0xD0u;
}

// The address a program stopped at is what the next start reads back, so the
// two always move together.
bool runMicroProgram(Vu& state, Memory& memory, Gif* gif, u32 unit, u32 start);

}  // namespace uee
