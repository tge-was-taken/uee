#pragma once

#include "elf.h"
#include "instruction.h"
#include "memory.h"
#include "types.h"

#include <map>
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

class Translator {
public:
    Translator(Memory& memory, const Program& program);
    virtual ~Translator() = default;

    std::string run();

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
    void claim(u32 function);
    std::vector<u32> successorsOf(u32 leader);
    bool owns(u32 function, u32 target) const;
    std::string go(u32 target, u32 function) const;
    static void dropJumpToNext(std::string& text, u32 next);
    static std::string annotate(const std::string& statement, u32 pc, Instruction insn,
                                s32 depth);
    std::string stepAt(u32 pc, s32 depth) const;
    std::string emitBeforeInstructionAt(s32 depth) const;
    bool delaySlotTransfers(u32 pc);
    std::string emitTransferringSlot(const std::string& pending, u32 pc, s32 depth) const;
    std::string counted(const std::string& body) const;
    std::string emitBlock(u32 leader, u32 function);
    std::string emitExit(const ControlFlow& cf, Op op, Instruction insn, u32 pc, u32 function);
    std::string render();

    static constexpr s32 kCommentColumn = 60;

    Memory& mMemory;
    const Program& mProgram;
    std::vector<bool> mCovered;
    std::set<u32> mFunctions;
    std::set<u32> mLeaders;
    std::map<u32, std::set<u32>> mOwned;
    std::map<u32, std::vector<u32>> mJumpTables;
    std::set<u32> mTableWords;
    std::set<u32> mWalked;
    std::vector<u32> mSeeds;
    u32 mInlineInstructions = 0;
};

}  // namespace uee
