#pragma once

#include "elf.h"
#include "instruction.h"
#include "memory.h"
#include "types.h"

#include <map>
#include <unordered_map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace uee {

std::string_view regName(u32 index);

struct Trap {
    std::string statement;
    bool continues = false;
};

// The source files a program was split into, and the declarations they share.
struct TranslatedSource {
    std::string declarations;
    std::vector<std::string> units;
};

class Translator {
public:
    Translator(Memory& memory, const Program& program);
    virtual ~Translator() = default;

    // One unit per function costs 30x the CPU and 4x the machine code.
    TranslatedSource run(usize units, std::string_view declarationsHeader);

protected:
    virtual std::string_view getMachineType() const = 0;
    virtual std::string_view getContextType() const = 0;
    virtual std::string_view getStateName() const = 0;
    virtual std::string_view getHandlersHeader() const = 0;
    virtual std::string_view getRuntimeHeader() const = 0;

    virtual std::string readGpr(u32 reg) const = 0;
    virtual std::string writeGpr(u32 reg, u32 value) const = 0;
    virtual std::optional<std::string> emitOne(Instruction insn) const = 0;
    virtual std::optional<std::string> emitBranchCondition(Op op, Instruction insn) const = 0;
    virtual std::optional<Trap> emitTrap(Instruction insn, Instruction previous,
                                         u32 pc) const = 0;

    virtual std::string emitBeforeInstruction() const { return {}; }
    virtual std::string countInstructions(u32 count) const;

    // The namespace opens in every unit; the module is defined in one.
    virtual std::string emitNamespaceOpen() const { return {}; }
    virtual std::string emitNamespaceClose() const { return {}; }
    virtual std::string emitBeforeNamespace() const { return {}; }
    virtual std::string emitDefinitions() const { return {}; }

private:
    bool inCode(u32 address) const;
    void seed(u32 function);
    void walkSeeds();
    void cover(u32 pc);
    bool reached(u32 pc) const;
    bool allocatesFrame(u32 pc);
    void seedFromDataPointers();
    void seedFromCodeConstants();
    void sweepUnreached();
    void promotePrologues();
    std::vector<u32> recoverJumpTable(u32 jrPc, u32 source);
    std::optional<Instruction> fetch(u32 pc);
    void discoverFrom(std::vector<u32> seeds);
    void partitionBlocks();
    std::vector<u32> claimFrom(u32 function);
    const std::vector<u32>& successorsOf(u32 blockStart);
    std::vector<u32> computeSuccessors(u32 blockStart);
    bool owns(u32 function, u32 target) const;
    u32 wordOf(u32 pc) const { return (pc - mProgram.codeStart) / 4; }
    std::string go(u32 target, u32 function) const;
    static void dropJumpToNext(std::string& text, u32 next);
    static std::string annotate(const std::string& statement, u32 pc, Instruction insn,
                                s32 depth);
    std::string stepAt(u32 pc, s32 depth) const;
    std::string emitBeforeInstructionAt(s32 depth) const;
    bool delaySlotTransfers(u32 pc);
    std::string emitTransferringSlot(const std::string& pending, u32 pc, s32 depth) const;
    std::string counted(const std::string& body) const;
    std::string emitBlock(u32 blockStart, u32 function);
    std::string emitExit(const ControlFlow& cf, Op op, Instruction insn, u32 pc, u32 function);
    std::string emitFunction(u32 function, const std::vector<u32>& blockStarts);
    std::string emitDeclarations() const;
    std::string emitDispatch() const;
    TranslatedSource render(usize units, std::string_view declarationsHeader);

    static constexpr s32 kCommentColumn = 60;

    Memory& mMemory;
    const Program& mProgram;
    std::vector<bool> mCovered;
    std::set<u32> mFunctions;
    std::set<u32> mBlockStarts;
    std::map<u32, std::vector<u32>> mOwned;
    // Which function each block belongs to, so a block belongs to exactly one.
    // No entry is kNoOwner.
    static constexpr u32 kNoOwner = 0xFFFFFFFFu;
    std::vector<u32> mOwner;
    std::map<u32, std::vector<u32>> mSuccessors;
    std::map<u32, std::vector<u32>> mJumpTables;
    std::set<u32> mTableWords;
    std::set<u32> mWalked;
    std::vector<u32> mSeeds;
    u32 mInlineInstructions = 0;
};

}  // namespace uee
