#pragma once

#include "elf.h"
#include "instruction.h"
#include "memory.h"
#include "types.h"

#include <map>
#include <span>
#include <optional>
#include <set>
#include <vector>

namespace uee {

class CodeMap {
public:
    static constexpr u32 kNoOwner = 0xFFFFFFFFu;

    // An IRX names its in its export table; nothing reaches them from the entry.
    CodeMap(Memory& memory, const Program& program, std::span<const u32> entries = {});

    // Address order, so functions that run together stay together.
    const std::map<u32, std::vector<u32>>& getBlocksByFunction() const { return mOwned; }
    const std::vector<u32>* getJumpTable(u32 pc) const;
    const std::vector<u32>& getSuccessors(u32 blockStart) const;

    // Its blocks may be entered any way at all.
    bool hasOpenJump(u32 function) const;

    bool isBlockStart(u32 pc) const { return mBlockStarts.count(pc) != 0; }
    bool isLoopHeader(u32 pc) const { return mLoopHeaders.count(pc) != 0; }

    // Naming every block instead costs the compiler a switch per function
    // large enough to run it out of memory.
    bool isResumePoint(u32 pc) const {
        return mCallReturns.count(pc) != 0 || mLoopHeaders.count(pc) != 0;
    }
    bool isFunction(u32 pc) const { return mFunctions.count(pc) != 0; }
    bool hasBody(u32 function) const { return mOwned.count(function) != 0; }
    // Nothing can read the clock part way through such a function.
    bool runsToCompletion(u32 function) const;

    // A function that does nothing but return. Calling one changes only the
    // clock, so a caller pays the count instead.
    std::optional<u32> countIfReturnOnly(u32 function) const {
        const auto found = mReturnOnly.find(function);
        return found == mReturnOnly.end() ? std::nullopt : std::optional(found->second);
    }

    // Past the last word the function owns, for hashing what it is made of.
    u32 getFunctionEnd(u32 function) const;

    bool owns(u32 function, u32 address) const;
    bool inCode(u32 address) const;

private:
    void seed(u32 function);
    void walkSeeds();
    void cover(u32 pc);
    bool reached(u32 pc) const;
    bool allocatesFrame(u32 pc);
    void seedFromSymbols();
    bool startsAfterFunction(u32 pc);
    bool savesReturnAddress(u32 pc);

    // A trace that runs the whole cap swept through data rather than reaching an end.
    struct Extent {
        u32 end = 0;
        // False when it ran out of code or hit the cap instead.
        bool ends = false;
        bool capped = false;
        // Data almost never forms a return encoding; a jump it does.
        bool returns = false;
        u32 handsOverTo = 0;
    };
    bool endsAsFunction(const Extent& extent) const;
    Extent traceExtent(u32 start);
    void sweepGaps();
    void promoteTailCalls();
    void findReturnOnlyFunctions();
    bool isPlainReturn(u32 pc);
    std::optional<u32> measureReturnOnly(u32 function, const std::vector<u32>& blocks);
    void dropInteriorHeads();
    u32 skipPadding(u32 pc, u32 limit);
    bool startsFunction(u32 pc);
    bool followsTerminator(u32 pc);
    bool pointsAtCode(u32 address);
    u32 backUpToStart(u32 pc);
    bool decodesThroughout(u32 start, u32 end);
    void seedFromDataPointers();
    void seedFromCodeConstants();
    void sweepUnreached();
    void promotePrologues();
    std::vector<u32> recoverJumpTable(u32 blockStart, u32 jrPc, u32 source);
    std::vector<u32> recoverJumpTableBackwards(u32 jrPc, u32 source);
    std::optional<Instruction> fetch(u32 pc);
    void discoverFrom(std::vector<u32> seeds);
    void partitionBlocks();
    std::vector<u32> claimFrom(u32 function);
    const std::vector<u32>& successorsOf(u32 blockStart);
    std::vector<u32> computeSuccessors(u32 blockStart);
    u32 wordOf(u32 pc) const { return (pc - mProgram.codeStart) / 4; }

    Memory& mMemory;
    const Program& mProgram;
    std::vector<bool> mCovered;
    std::set<u32> mFunctions;
    std::set<u32> mBlockStarts;
    std::map<u32, std::vector<u32>> mOwned;
    // Exactly one owner per block; no entry is kNoOwner.
    std::vector<u32> mOwner;
    std::map<u32, std::vector<u32>> mSuccessors;
    std::map<u32, std::vector<u32>> mJumpTables;
    std::set<u32> mCallReturns;
    std::set<u32> mLoopHeaders;
    std::set<u32> mOpenJumps;
    std::set<u32> mTableWords;
    // A branch target is never a function; a direct call's target always is.
    std::set<u32> mCalledDirectly;
    std::map<u32, u32> mReturnOnly;
    std::set<u32> mBranchTargets;
    std::set<u32> mWalked;
    std::vector<u32> mSeeds;
};

}  // namespace uee
