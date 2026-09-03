#pragma once

#include "block_values.h"
#include "code_map.h"
#include "elf.h"
#include "instruction.h"
#include "memory.h"
#include "symbols.h"
#include "types.h"

#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uee {

std::string_view regName(u32 index);

struct Trap {
    std::string statement;
    bool continues = false;
};

struct TranslatedSource {
    std::string declarations;
    std::vector<std::string> units;
};

class Translator {
public:
    Translator(Memory& memory, const Program& program, const Symbols* symbols,
               std::span<const u32> entries = {});
    virtual ~Translator() = default;

    // One unit per function costs 30x the CPU and 4x the machine code.
    TranslatedSource run(usize units, std::string_view declarationsHeader);

    void setHookable(const std::set<u32>& addresses) { mHookable = addresses; }
    void setProgramValues(const BlockValues& values) { mProgramValues = values; }

protected:
    std::string getParameters() const;
    std::string getParameterTypes() const;
    std::string getArguments() const;

    // A branch in every prologue, so only addresses a frontend claims get one.
    virtual std::string emitHookCheck(u32 function) const { return {}; }

    // Reaching one costs a run loop iteration, a module search and a lookup.
    virtual std::optional<std::string> emitInlinedCall(u32 target, u32 returnTo) const {
        return {};
    }

    Memory& mMemory;
    CodeMap mCode;
    const Program& mProgram;

    std::set<u32> mHookable;

    void noteCall(u32 caller, u32 callee) const { mCallers[callee].insert(caller); }
    void noteRenderKick(u32 pc) const { mRenderKickSites.insert(pc); }
    // The paths left to reimplement in the frontend.
    std::string reportUnhookedRenderPaths() const;

    mutable std::map<u32, std::set<u32>> mCallers;
    mutable std::set<u32> mRenderKickSites;

    virtual std::string_view getMachineType() const = 0;
    virtual std::string_view getContextType() const = 0;
    virtual std::string_view getStateName() const = 0;
    virtual std::string_view getHandlersHeader() const = 0;
    virtual std::string_view getRuntimeHeader() const = 0;

    virtual std::string readGpr(u32 reg) const = 0;
    virtual std::string writeGpr(u32 reg, u32 value) const = 0;
    virtual std::string writeConstant(u32 reg, u32 value) const = 0;
    virtual std::optional<std::string> emitOne(u32 pc, Instruction insn) const = 0;
    virtual std::optional<std::string> emitBranchCondition(Op op, Instruction insn) const = 0;
    // For what the encoding does not carry, such as which syscall a register names.
    virtual std::optional<Trap> emitTrap(Instruction insn, const BlockValues& values,
                                         u32 pc) const = 0;

    virtual std::string emitBeforeInstruction() const { return {}; }
    virtual LoadDelay getLoadDelay() const = 0;
    virtual std::string countInstructions(u32 count, u32 blockStart) const;
    virtual std::string countInstructionsOnly(u32 count) const;
    virtual std::string_view getStopFunction() const { return "stopAt"; }

    BlockValues mProgramValues;
    // Worked out once: what a call to each function can leave changed.
    std::map<u32, u32> mClobbered;

    std::optional<u32> getKnownValue(u32 pc, u32 reg) const;
    std::string emitBase(u32 pc, u32 reg) const;

    // A guest reaches a register window through a constant, recognised here.
    std::optional<Resolved> resolveAccess(u32 pc, u32 base, s32 offset) const;

    virtual std::string emitNamespaceOpen() const { return {}; }
    virtual std::string emitNamespaceClose() const { return {}; }
    virtual std::string emitBeforeNamespace() const { return {}; }
    virtual std::string emitDefinitions() const { return {}; }

private:
    std::string nameOf(u32 function) const;
    std::optional<Instruction> fetch(u32 pc);
    std::optional<std::string> emitInstruction(u32 pc, Instruction insn) const;
    std::string go(u32 target, u32 function) const;
    static void dropJumpToNext(std::string& text, u32 next);
    static std::string annotate(const std::string& statement, u32 pc, Instruction insn,
                                s32 depth);
    std::string stepAt(u32 pc, s32 depth) const;
    std::string emitBeforeInstructionAt(s32 depth) const;
    bool delaySlotTransfers(u32 pc);
    std::string emitTransferringSlot(const std::string& pending, u32 pc, s32 depth) const;
    std::string countedAt(u32 function, u32 blockStart, const std::string& body) const;
    std::string emitBlock(u32 blockStart, u32 function);
    std::string emitExit(const ControlFlow& cf, Op op, Instruction insn, u32 pc, u32 function);
    std::string emitFunction(u32 function, const std::vector<u32>& blockStarts);
    std::string emitReentry(u32 function, const std::vector<u32>& order) const;
    std::string emitDeclarations() const;
    std::string emitDispatch() const;
    TranslatedSource render(usize units, std::string_view declarationsHeader);

    static constexpr s32 kCommentColumn = 60;
    // A whole C++ mangled name makes the generated source unreadable.
    static constexpr usize kNameLimit = 48;

    const Symbols* mSymbols;
    std::map<u32, BlockValues> mBlockEntries;
    std::map<u32, BlockValues> mValues;
    std::map<u32, std::set<u32>> mDeadWritesByBlock;
    std::set<u32> mDeadWrites;
    bool mKeepsReturnAddress = false;
    u32 mInlineInstructions = 0;
};

}  // namespace uee
