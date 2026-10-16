#include "block_values.h"
#include "code_map.h"

#include <cstdio>
#include <cstdlib>

#include "instruction.h"

#include <algorithm>
#include <format>
#include <stdexcept>
#include <utility>

namespace uee {
namespace {

usize countCodeWords(const Program& program) {
    if (program.codeEnd <= program.codeStart) {
        throw std::runtime_error(std::format(
            "the program entered at {:08x} declares no executable range, so nothing in it "
            "can be decoded", program.entry));
    }
    return (program.codeEnd - program.codeStart) / 4;
}

}  // namespace

CodeMap::CodeMap(Memory& memory, const Program& program, std::span<const u32> entries)
    : mMemory(memory), mProgram(program), mCovered(countCodeWords(program)) {
    seed(mProgram.entry);
    for (const u32 entry : entries) {
        seed(entry);
    }
    walkSeeds();
    seedFromSymbols();
    walkSeeds();
    seedFromCodeConstants();
    walkSeeds();
    seedFromDataPointers();
    walkSeeds();
    sweepUnreached();
    sweepGaps();
    promoteTailCalls();
    promotePrologues();
    partitionBlocks();
    // Needs the ownership the partition works out.
    findReturnOnlyFunctions();
}

const std::vector<u32>* CodeMap::getJumpTable(u32 pc) const {
    const auto found = mJumpTables.find(pc);
    return found == mJumpTables.end() ? nullptr : &found->second;
}

const std::vector<u32>& CodeMap::getSuccessors(u32 blockStart) const {
    static const std::vector<u32> none;
    const auto found = mSuccessors.find(blockStart);
    return found == mSuccessors.end() ? none : found->second;
}

u32 CodeMap::getFunctionEnd(u32 function) const {
    const auto blocks = mOwned.find(function);
    if (blocks == mOwned.end() || blocks->second.empty()) {
        return function;
    }
    // Block length is not tracked; the next block's start is the only bound there is.
    const u32 lastBlock = *std::max_element(blocks->second.begin(), blocks->second.end());
    const auto next = mBlockStarts.upper_bound(lastBlock);
    return next != mBlockStarts.end() ? *next : lastBlock + 4;
}

void CodeMap::findReturnOnlyFunctions() {
    for (const auto& [function, blocks] : mOwned) {
        if (const std::optional<u32> count = measureReturnOnly(function, blocks)) {
            mReturnOnly[function] = *count;
        }
    }
}

bool CodeMap::isPlainReturn(u32 pc) {
    const std::optional<Instruction> insn = fetch(pc);
    if (!insn) {
        return false;
    }
    const ControlFlow cf = getControlFlow(getOp(*insn), *insn, pc);
    return cf.flow == Flow::JumpRegister && cf.link == 0 && cf.source == reg::ra;
}

// A function whose one block is padding, then `jr ra`, then a nop in its delay
// slot. The count it answers with covers all three.
std::optional<u32> CodeMap::measureReturnOnly(u32 function, const std::vector<u32>& blocks) {
    if (blocks.size() != 1) {
        return std::nullopt;
    }
    for (u32 pc = function; pc + 8 <= mProgram.codeEnd; pc += 4) {
        const std::optional<Instruction> insn = fetch(pc);
        if (!insn) {
            return std::nullopt;
        }
        if (insn->word == 0) {
            continue;
        }
        const std::optional<Instruction> slot = fetch(pc + 4);
        if (!isPlainReturn(pc) || !slot || slot->word != 0) {
            return std::nullopt;
        }
        return (pc + 8 - function) / 4;
    }
    return std::nullopt;
}

bool CodeMap::runsToCompletion(u32 function) const {
    const auto blocks = mOwned.find(function);
    if (blocks == mOwned.end()) {
        return false;
    }
    for (u32 block : blocks->second) {
        if (mLoopHeaders.count(block) != 0 || mCallReturns.count(block) != 0) {
            return false;
        }
    }
    return true;
}

bool CodeMap::hasOpenJump(u32 function) const {
    for (u32 pc : mOpenJumps) {
        if (owns(function, pc)) {
            return true;
        }
    }
    return false;
}

// A lui/addiu pair can name an address close enough to the top to wrap.
bool CodeMap::inCode(u32 address) const {
    return (address & 3) == 0 && address >= mProgram.codeStart &&
           address < mProgram.codeEnd && mProgram.codeEnd - address >= 4;
}

void CodeMap::seed(u32 function) {
    mFunctions.insert(function);
    mBlockStarts.insert(function);
    mSeeds.push_back(function);
}

void CodeMap::walkSeeds() {
    discoverFrom(std::exchange(mSeeds, {}));
}

void CodeMap::cover(u32 pc) {
    if (inCode(pc)) {
        mCovered[(pc - mProgram.codeStart) / 4] = true;
    }
}

bool CodeMap::reached(u32 pc) const {
    return mCovered[(pc - mProgram.codeStart) / 4];
}

// Absorbing one into its caller would nest a guest frame in a host frame
// that does not own it.
bool CodeMap::allocatesFrame(u32 pc) {
    const std::optional<Instruction> insn = fetch(pc);
    return insn && getOp(*insn) == Op::addiu && insn->rt() == reg::sp &&
           insn->rs() == reg::sp && insn->simm() < 0;
}

void CodeMap::seedFromSymbols() {
    for (u32 function : mProgram.functions) {
        seed(function);
    }
}

bool CodeMap::pointsAtCode(u32 address) {
    const std::optional<Instruction> word = fetch(address);
    return word && inCode(word->word) && (word->word & 3) == 0;
}

void CodeMap::seedFromDataPointers() {
    const u32 first = (mProgram.imageStart + 3) & ~3u;
    for (u32 address = first; address + 4 <= mProgram.imageEnd; address += 4) {
        if (address >= mProgram.codeStart && address < mProgram.codeEnd) {
            continue;
        }
        if (mTableWords.count(address)) {
            continue;
        }
        const std::optional<Instruction> word = fetch(address);
        if (!word) {
            return;
        }
        if (inCode(word->word) && !mFunctions.count(word->word) &&
            allocatesFrame(word->word)) {
            seed(word->word);
        }
    }
}

// Landing on an instruction is the only evidence the encoding leaves.
void CodeMap::seedFromCodeConstants() {
    u32 upper[32] = {};
    bool haveUpper[32] = {};
    bool lastTransfers = false;
    bool slotEnded = false;
    for (u32 pc = mProgram.codeStart; pc + 4 <= mProgram.codeEnd; pc += 4) {
        const std::optional<Instruction> insn = fetch(pc);
        if (!insn) {
            return;
        }
        const Op op = getOp(*insn);
        const bool transfers = getControlFlow(op, *insn, pc).flow != Flow::Next;
        // A delay slot still runs in the block its transfer ends, and a call
        // often finishes its last argument there.
        if (std::exchange(slotEnded, std::exchange(lastTransfers, transfers))) {
            std::fill(std::begin(haveUpper), std::end(haveUpper), false);
        }
        if (transfers) {
            continue;
        }
        if (op == Op::lui) {
            upper[insn->rt()] = insn->imm() << 16;
            haveUpper[insn->rt()] = true;
            continue;
        }
        if (op == Op::addiu || op == Op::daddiu || op == Op::ori) {
            const u32 low = op == Op::ori ? insn->imm() : static_cast<u32>(insn->simm());
            const u32 address = upper[insn->rs()] + low;
            // A callback registered this way is reached no other way, and keeps
            // no frame if it is small.
            if (haveUpper[insn->rs()] && inCode(address) && !reached(address) &&
                !mFunctions.count(address)) {
                const Extent extent = traceExtent(address);
                if (decodesThroughout(address, extent.end)) {
                    seed(address);
                }
            }
        }
        haveUpper[insn->rt()] = false;
        if ((insn->word >> 26) == 0) {
            haveUpper[insn->rd()] = false;
        }
    }
}

// The word before a function is the delay slot of the return that ended the one
// before it, or the padding between them.
bool CodeMap::startsAfterFunction(u32 pc) {
    const std::optional<Instruction> padding = fetch(pc - 4);
    if (padding && padding->word == 0) {
        return true;
    }
    const std::optional<Instruction> before = fetch(pc - 8);
    if (!before) {
        return false;
    }
    // A jump is not enough: one inside a function looks like one that ends it.
    const ControlFlow cf = getControlFlow(getOp(*before), *before, pc - 8);
    return cf.flow == Flow::JumpRegister && cf.link == 0 && cf.source == reg::ra;
}

// A bare stack adjustment also appears part way through a function.
bool CodeMap::savesReturnAddress(u32 pc) {
    constexpr u32 kPrologueWindow = 16;
    bool allocated = false;
    for (u32 at = pc; at < pc + 4 * kPrologueWindow && at + 4 <= mProgram.codeEnd; at += 4) {
        const std::optional<Instruction> insn = fetch(at);
        if (!insn) {
            return false;
        }
        const Op op = getOp(*insn);
        if (op == Op::addiu && insn->rt() == reg::sp && insn->rs() == reg::sp &&
            insn->simm() < 0) {
            allocated = true;
        }
        if ((op == Op::sw || op == Op::sd || op == Op::sq) && insn->rt() == reg::ra) {
            return allocated;
        }
    }
    return false;
}

// A terminator ends the function only where no branch seen reaches past it.
CodeMap::Extent CodeMap::traceExtent(u32 start) {
    constexpr u32 kMaxExtent = 0x8000;
    Extent extent{start, false, false};
    u32 furthest = start;
    u32 pc = start;
    while (pc + 4 <= mProgram.codeEnd) {
        if (pc - start >= kMaxExtent) {
            extent.capped = true;
            return extent;
        }
        const std::optional<Instruction> insn = fetch(pc);
        if (!insn) {
            return extent;
        }
        const Op op = getOp(*insn);
        const ControlFlow cf = getControlFlow(op, *insn, pc);
        const auto reaches = [&](u32 target) {
            if (target > furthest && target - start < kMaxExtent) {
                furthest = target;
            }
        };

        if (cf.flow == Flow::Next) {
            pc += 4;
            extent.end = pc;
            continue;
        }
        if (cf.flow == Flow::Trap) {
            pc += 4;
            extent.end = pc;
            continue;
        }
        // A call comes back, so it never ends the function.
        if (cf.link != 0) {
            pc += 8;
            extent.end = pc;
            continue;
        }
        if (cf.flow == Flow::Branch || cf.flow == Flow::BranchLikely) {
            reaches(cf.target);
            pc += 8;
            extent.end = pc;
            if (!alwaysBranches(op, *insn)) {
                reaches(pc);
                continue;
            }
            if (pc > furthest) {
                extent.ends = true;
                return extent;
            }
            continue;
        }
        if (cf.flow == Flow::Jump) {
            reaches(cf.target);
            pc += 8;
            extent.end = pc;
            if (pc > furthest) {
                extent.ends = true;
                extent.handsOverTo = cf.target;
                return extent;
            }
            continue;
        }
        if (cf.source == reg::ra) {
            extent.returns = true;
        } else {
            if (const auto table = mJumpTables.find(pc); table != mJumpTables.end()) {
                for (u32 target : table->second) {
                    reaches(target);
                }
            }
        }
        pc += 8;
        extent.end = pc;
        if (pc > furthest) {
            extent.ends = true;
            return extent;
        }
    }
    return extent;
}

// A jump into nowhere is what data decodes as.
bool CodeMap::endsAsFunction(const Extent& extent) const {
    if (extent.capped || !extent.ends) {
        return false;
    }
    return extent.returns || (extent.handsOverTo != 0 &&
                              mFunctions.count(extent.handsOverTo) != 0);
}

bool CodeMap::decodesThroughout(u32 start, u32 end) {
    for (u32 pc = start; pc + 4 <= end; pc += 4) {
        const std::optional<Instruction> insn = fetch(pc);
        if (!insn || getOp(*insn) == Op::Invalid) {
            return false;
        }
    }
    return true;
}

u32 CodeMap::skipPadding(u32 pc, u32 limit) {
    while (pc + 4 <= limit) {
        const std::optional<Instruction> word = fetch(pc);
        if (!word || word->word != 0) {
            return pc;
        }
        pc += 4;
    }
    return pc;
}

// Weaker than a return, and enough to tell a nested function from a piece.
bool CodeMap::followsTerminator(u32 pc) {
    const std::optional<Instruction> padding = fetch(pc - 4);
    if (padding && padding->word == 0) {
        return true;
    }
    const std::optional<Instruction> before = fetch(pc - 8);
    if (!before) {
        return false;
    }
    const Op op = getOp(*before);
    const ControlFlow cf = getControlFlow(op, *before, pc - 8);
    if (cf.link != 0) {
        return false;
    }
    return cf.flow == Flow::Jump || cf.flow == Flow::JumpRegister ||
           ((cf.flow == Flow::Branch || cf.flow == Flow::BranchLikely) &&
            alwaysBranches(op, *before));
}

// The compiler's entry shape, or nothing at all for a leaf.
bool CodeMap::startsFunction(u32 pc) {
    return allocatesFrame(pc) || savesReturnAddress(pc) || startsAfterFunction(pc);
}

// A function sets up before allocating its frame, so that is not its start.
u32 CodeMap::backUpToStart(u32 pc) {
    constexpr u32 kSetupInstructions = 64;
    u32 start = pc;
    for (u32 i = 0; i < kSetupInstructions; ++i) {
        if (start <= mProgram.codeStart || startsAfterFunction(start)) {
            return start;
        }
        if (reached(start - 4)) {
            return start;
        }
        start -= 4;
    }
    return start;
}

// Where two overlap the earlier is the function, unless a call names the later.
void CodeMap::dropInteriorHeads() {
    const std::vector<u32> heads(mFunctions.begin(), mFunctions.end());
    std::vector<u32> interior;
    for (usize i = 0; i < heads.size(); ++i) {
        const Extent extent = traceExtent(heads[i]);
        if (extent.capped || !extent.ends) {
            continue;
        }
        for (usize j = i + 1; j < heads.size() && heads[j] < extent.end; ++j) {
            if (heads[j] != mProgram.entry && mCalledDirectly.count(heads[j]) == 0 &&
                !followsTerminator(heads[j])) {
                interior.push_back(heads[j]);
            }
        }
    }
    for (u32 pc : interior) {
        mFunctions.erase(pc);
    }
}

// Absorbing a tail call's target leaves it no name to be called or hooked by.
void CodeMap::promoteTailCalls() {
    for (bool changed = true; changed;) {
        changed = false;
        const std::vector<u32> heads(mFunctions.begin(), mFunctions.end());
        for (u32 head : heads) {
            const Extent extent = traceExtent(head);
            if (extent.capped) {
                continue;
            }
            for (u32 pc = head; pc + 4 <= extent.end; pc += 4) {
                const std::optional<Instruction> insn = fetch(pc);
                if (!insn) {
                    break;
                }
                const Op op = getOp(*insn);
                const ControlFlow cf = getControlFlow(op, *insn, pc);
                if (cf.flow != Flow::Jump || cf.link != 0) {
                    continue;
                }
                if (cf.target >= head && cf.target < extent.end) {
                    continue;
                }
                if (inCode(cf.target) && mFunctions.count(cf.target) == 0) {
                    seed(cf.target);
                    changed = true;
                }
            }
        }
        if (changed) {
            walkSeeds();
        }
    }
}

// A function nothing calls directly is only ever found in a gap.
void CodeMap::sweepGaps() {
    for (bool changed = true; changed;) {
        changed = false;
        std::vector<u32> heads(mFunctions.begin(), mFunctions.end());
        heads.insert(heads.begin(), mProgram.codeStart);
        for (usize i = 0; i < heads.size(); ++i) {
            const u32 limit = i + 1 < heads.size() ? heads[i + 1] : mProgram.codeEnd;
            u32 pc = i == 0 ? mProgram.codeStart
                            : skipPadding(traceExtent(heads[i]).end, limit);
            pc = skipPadding(pc, limit);
            while (pc + 4 <= limit) {
                const Extent extent = traceExtent(pc);
                if (extent.end <= pc) {
                    break;
                }
                const u32 start = backUpToStart(pc);
                const bool take = mFunctions.count(start) == 0 && endsAsFunction(extent) &&
                                  decodesThroughout(pc, extent.end);
                if (take) {
                    seed(start);
                    changed = true;
                }
                const u32 next = skipPadding(extent.end, limit);
                if (next <= pc) {
                    break;
                }
                pc = next;
            }
        }
        if (changed) {
            walkSeeds();
        }
    }
}

void CodeMap::sweepUnreached() {
    for (u32 pc = mProgram.codeStart; pc + 4 <= mProgram.codeEnd; pc += 4) {
        if (reached(pc)) {
            continue;
        }
        if (!allocatesFrame(pc)) {
            continue;
        }
        const u32 start = backUpToStart(pc);
        const Extent extent = traceExtent(start);
        if (!endsAsFunction(extent) || !decodesThroughout(start, extent.end)) {
            continue;
        }
        seed(start);
        walkSeeds();
    }
}

void CodeMap::promotePrologues() {
    for (u32 blockStart : mBlockStarts) {
        if (allocatesFrame(blockStart)) {
            mFunctions.insert(blockStart);
        }
    }
}

// Recovered, the jump stays local; a host call would make it a new frame.
std::vector<u32> CodeMap::recoverJumpTableBackwards(u32 jrPc, u32 source) {
    u32 base = 0, count = 0, pendingLow = 0, pendingReg = 32, loadBase = 32;
    s32 loadOffset = 0;
    bool haveBase = false, haveCount = false, haveOffset = false;
    for (s32 back = 1; back <= 16 && !(haveBase && haveCount); ++back) {
        const std::optional<Instruction> insn = fetch(jrPc - 4 * static_cast<u32>(back));
        if (!insn) {
            break;
        }
        switch (getOp(*insn)) {
            case Op::lw:
                if (insn->rt() == source && !haveOffset) {
                    loadOffset = insn->simm();
                    loadBase = insn->rs();
                    haveOffset = true;
                }
                break;
            case Op::addiu:
                if (insn->rt() == insn->rs()) {
                    pendingLow = static_cast<u32>(insn->simm());
                    pendingReg = insn->rt();
                }
                break;
            case Op::lui:
                if (insn->rt() == pendingReg) {
                    base = (insn->imm() << 16) + pendingLow;
                    haveBase = true;
                } else if (haveOffset && insn->rt() == loadBase) {
                    base = insn->imm() << 16;
                    haveBase = true;
                }
                break;
            case Op::sltiu:
                count = insn->imm();
                haveCount = true;
                break;
            default:
                break;
        }
    }
    std::vector<u32> targets;
    if (!haveBase || !haveCount || !haveOffset || count == 0 || count > 1024) {
        return targets;
    }
    const u32 table = base + static_cast<u32>(loadOffset);
    for (u32 i = 0; i < count; ++i) {
        const std::optional<Instruction> entry = fetch(table + 4 * i);
        if (!entry || !inCode(entry->word)) {
            return {};
        }
        targets.push_back(entry->word);
    }
    for (u32 i = 0; i < count; ++i) {
        mTableWords.insert(table + 4 * i);
    }
    return targets;
}

// The same evaluator the emitter uses, so any shape of address works.
std::vector<u32> CodeMap::recoverJumpTable(u32 blockStart, u32 jrPc, u32 source) {
    BlockValues values;
    std::optional<u32> table;
    std::optional<u32> count;
    for (u32 pc = blockStart; pc <= jrPc; pc += 4) {
        const std::optional<Instruction> insn = fetch(pc);
        if (!insn) {
            return {};
        }
        const Op op = getOp(*insn);
        if (op == Op::sltiu && insn->imm() != 0) {
            count = insn->imm();
        }
        if (op == Op::lw && insn->rt() == source) {
            table = values.getAddress(*insn);
        }
        values.apply(op, *insn);
    }

    // The load can sit in an earlier block, which a forward walk never sees.
    if (!table) {
        return recoverJumpTableBackwards(jrPc, source);
    }
    std::vector<u32> targets;
    if (!count || *count > 1024) {
        return targets;
    }
    for (u32 i = 0; i < *count; ++i) {
        const std::optional<Instruction> entry = fetch(*table + 4 * i);
        if (!entry || !inCode(entry->word)) {
            return {};
        }
        targets.push_back(entry->word);
    }
    for (u32 i = 0; i < *count; ++i) {
        mTableWords.insert(*table + 4 * i);
    }
    return targets;
}

std::optional<Instruction> CodeMap::fetch(u32 pc) {
    const std::optional<u32> word = mMemory.peek32(pc);
    return word ? std::optional(Instruction{*word}) : std::nullopt;
}

void CodeMap::discoverFrom(std::vector<u32> seeds) {
    std::vector<u32> pending = std::move(seeds);
    std::set<u32>& walked = mWalked;
    while (!pending.empty()) {
        const u32 start = pending.back();
        pending.pop_back();
        if (!walked.insert(start).second) {
            continue;
        }
        const auto reach = [&](u32 target) {
            if (!inCode(target)) {
                return;
            }
            mBlockStarts.insert(target);
            pending.push_back(target);
        };
        u32 pc = start;
        for (;;) {
            const std::optional<Instruction> insn = fetch(pc);
            if (!insn) {
                break;
            }
            cover(pc);
            const Op op = getOp(*insn);
            const ControlFlow cf = getControlFlow(op, *insn, pc);
            if (cf.flow == Flow::Next || (cf.flow == Flow::Trap && op == Op::syscall)) {
                pc += 4;
                if (mBlockStarts.count(pc)) {
                    break;
                }
                continue;
            }
            if (cf.flow != Flow::Trap) {
                cover(pc + 4);
            }
            if (cf.flow == Flow::Branch || cf.flow == Flow::BranchLikely) {
                mBranchTargets.insert(cf.target);
                reach(cf.target);
                if (!alwaysBranches(op, *insn)) {
                    reach(pc + 8);
                }
            } else if (cf.flow == Flow::Jump) {
                if (cf.link != 0) {
                    if (inCode(cf.target)) {
                        mFunctions.insert(cf.target);
                        mCalledDirectly.insert(cf.target);
                    }
                    reach(cf.target);
                    reach(pc + 8);
                    mCallReturns.insert(pc + 8);
                } else {
                    reach(cf.target);
                }
            } else if (cf.flow == Flow::JumpRegister) {
                if (cf.link != 0) {
                    reach(pc + 8);
                    mCallReturns.insert(pc + 8);
                } else if (cf.source != 31) {
                    const std::vector<u32> targets = recoverJumpTable(start, pc, cf.source);
                    mJumpTables[pc] = targets;
                    if (targets.empty()) {
                        mOpenJumps.insert(pc);
                    }
                    for (u32 target : targets) {
                        reach(target);
                    }
                }
            }
            break;
        }
    }
}

// A block two functions reach becomes a function, rather than a copy in each.
void CodeMap::partitionBlocks() {
    for (;;) {
        mOwner.assign(mCovered.size(), kNoOwner);
        std::vector<u32> contested;
        for (u32 function : mFunctions) {
            for (u32 blockStart : claimFrom(function)) {
                contested.push_back(blockStart);
            }
        }
        if (contested.empty()) {
            break;
        }
        mFunctions.insert(contested.begin(), contested.end());
    }

    for (u32 blockStart : mBlockStarts) {
        const u32 owner = mOwner[wordOf(blockStart)];
        if (owner != kNoOwner) {
            mOwned[owner].push_back(blockStart);
        }
        // A branch back to an address already passed is what makes a loop.
        for (u32 successor : successorsOf(blockStart)) {
            if (successor <= blockStart) {
                mLoopHeaders.insert(successor);
            }
        }
    }
}

// Reachable without passing another entry, plus the ones already owned.
std::vector<u32> CodeMap::claimFrom(u32 function) {
    std::vector<u32> contested;
    std::vector<u32> pending{function};
    while (!pending.empty()) {
        const u32 blockStart = pending.back();
        pending.pop_back();
        if (!mBlockStarts.count(blockStart)) {
            continue;
        }
        if (blockStart != function && mFunctions.count(blockStart)) {
            continue;
        }
        u32& owner = mOwner[wordOf(blockStart)];
        if (owner == function) {
            continue;
        }
        if (owner != kNoOwner) {
            contested.push_back(blockStart);
            continue;
        }
        owner = function;
        for (u32 successor : successorsOf(blockStart)) {
            pending.push_back(successor);
        }
    }
    return contested;
}

// Claiming revisits blocks, and nothing they depend on moves after discovery.
const std::vector<u32>& CodeMap::successorsOf(u32 blockStart) {
    const auto found = mSuccessors.find(blockStart);
    if (found != mSuccessors.end()) {
        return found->second;
    }
    return mSuccessors.emplace(blockStart, computeSuccessors(blockStart)).first->second;
}

std::vector<u32> CodeMap::computeSuccessors(u32 blockStart) {
    std::vector<u32> out;
    u32 pc = blockStart;
    for (;;) {
        const std::optional<Instruction> insn = fetch(pc);
        if (!insn) {
            return out;
        }
        const Op op = getOp(*insn);
        const ControlFlow cf = getControlFlow(op, *insn, pc);
        if (cf.flow == Flow::Next || (cf.flow == Flow::Trap && op == Op::syscall)) {
            pc += 4;
            if (mBlockStarts.count(pc)) {
                out.push_back(pc);
                return out;
            }
            continue;
        }
        if (cf.flow == Flow::Branch || cf.flow == Flow::BranchLikely) {
            out.push_back(cf.target);
            if (!alwaysBranches(op, *insn)) {
                out.push_back(pc + 8);
            }
        } else if (cf.flow == Flow::Jump) {
            out.push_back(cf.link != 0 ? pc + 8 : cf.target);
        } else if (cf.flow == Flow::JumpRegister) {
            if (cf.link != 0) {
                out.push_back(pc + 8);
            } else if (const auto table = mJumpTables.find(pc); table != mJumpTables.end()) {
                for (u32 target : table->second) {
                    out.push_back(target);
                }
            }
        }
        return out;
    }
}

bool CodeMap::owns(u32 function, u32 target) const {
    return inCode(target) && mOwner[wordOf(target)] == function;
}

}  // namespace uee
