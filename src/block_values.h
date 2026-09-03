#pragma once

#include "instruction.h"
#include "memory.h"
#include "types.h"

#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace uee {

class CodeMap;

struct KnownWrite {
    u32 reg;
    u32 value;
};

// at, v0-v1, a0-a3, t0-t9, k0-k1 and ra: what o32 lets a callee leave changed.
// zero, s0-s7, gp, sp and fp are the ones it must put back.
constexpr u32 kCallerSaved = 0x8F00FFFEu;

class ReadOnlyImage {
public:
    ReadOnlyImage(Memory& memory, std::vector<std::pair<u32, u32>> ranges)
        : mMemory(&memory), mRanges(std::move(ranges)) {}

    std::optional<u32> read32(u32 address) const;

private:
    Memory* mMemory;
    std::vector<std::pair<u32, u32>> mRanges;
};

class BlockValues {
public:
    std::optional<u32> getRegister(u32 reg) const;

    std::optional<u32> getAddress(Instruction insn) const;

    // Which region an access reaches when the index is unknown, not which word.
    std::optional<u32> getBase(u32 reg) const;
    std::optional<u32> getAddressBase(Instruction insn) const;

    void apply(Op op, Instruction insn, u32 callClobber = kCallerSaved);

    // True when the meet changed anything.
    bool meet(const BlockValues& other);

    // For a value no walk over one function can establish.
    void hold(u32 reg, u32 value) { setRegister(reg, value); }

    void readFrom(const ReadOnlyImage* image) { mImage = image; }

    friend bool operator==(const BlockValues&, const BlockValues&) = default;

private:
    // One past the last register, so no register index is ever equal to it.
    static constexpr u32 kNoRegister = 32;

    void applyWrites(Op op, Instruction insn);

    // Set for the next call applied, then left alone.
    u32 mCallClobber = kCallerSaved;
    void setRegister(u32 reg, u32 value);
    void setBase(u32 reg, u32 base);
    void forget(u32 reg);
    void forgetAll();

    u32 mValues[32] = {};
    u32 mKnown = 1u;  // register zero always reads as zero
    u32 mBases[32] = {};
    u32 mBased = 0;
    const ReadOnlyImage* mImage = nullptr;
    // Tracked on both cores, though only the R3000A delays a load.
    u32 mLoadedPreviously = kNoRegister;
};

std::optional<KnownWrite> foldConstant(const BlockValues& values, Op op, Instruction insn);

std::map<u32, BlockValues> analyseBlock(Memory& memory, const CodeMap& code, u32 blockStart,
                                        const BlockValues& entry);

// Worked out over the call graph, so a callee that touches less lets its
// caller keep the rest.
std::map<u32, u32> findClobberedRegisters(Memory& memory, const CodeMap& code);

// Seeding only the entry block loses a program-wide constant at the first
// resume point.
std::map<u32, BlockValues> analyseFunction(Memory& memory, const CodeMap& code, u32 function,
                                           const std::vector<u32>& blocks,
                                           const BlockValues& always = {},
                                           const std::map<u32, u32>* clobbered = nullptr);

// The R3000A delays a load by an instruction; the R5900 does not.
enum class LoadDelay {
    None,
    OneInstruction,
};

// A block that hands its registers outside the function is asked for all.
std::map<u32, std::set<u32>> findDeadWritesInFunction(
    Memory& memory, const CodeMap& code, const std::vector<u32>& blocks,
    const std::map<u32, BlockValues>& entries, LoadDelay delay);

// True only when nothing in the function, or anything it calls, writes ra.
bool keepsReturnAddress(Memory& memory, const CodeMap& code, u32 function,
                        const std::vector<u32>& blocks);

}  // namespace uee
