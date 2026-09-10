#include "translator.h"

#include "log.h"
#include "guest_replacement.h"

#include <cstdio>
#include <cstdlib>

#include "block_values.h"
#include "code_map.h"
#include "instruction.h"

#include <algorithm>
#include <format>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace uee {
namespace {
LoggerChannel log("translate");
}

namespace {

const BlockValues kNothingKnown;

}  // namespace

Translator::Translator(Memory& memory, const Program& program, const Symbols* symbols,
                       std::span<const u32> entries)
    : mMemory(memory), mCode(memory, program, entries), mProgram(program),
      mSymbols(symbols), mClobbered(findClobberedRegisters(memory, mCode)) {}

// The address stays in front, so a repeated or missing name still labels one.
std::string Translator::nameOf(u32 function) const {
    const std::optional<std::string_view> name =
        mSymbols != nullptr ? mSymbols->nameAt(function) : std::nullopt;
    if (!name) {
        return std::format("func_{:08x}", function);
    }
    std::string suffix;
    for (char c : name->substr(0, kNameLimit)) {
        suffix += (std::isalnum(static_cast<unsigned char>(c)) != 0) ? c : '_';
    }
    return std::format("func_{:08x}_{}", function, suffix);
}

std::string Translator::countInstructions(u32, u32) const {
    return {};
}

std::string Translator::countInstructionsOnly(u32) const {
    return {};
}

TranslatedSource Translator::run(usize units, std::string_view declarationsHeader) {
    return render(units, declarationsHeader);
}

std::optional<Instruction> Translator::fetch(u32 pc) {
    const std::optional<u32> word = mMemory.peek32(pc);
    return word ? std::optional(Instruction{*word}) : std::nullopt;
}

std::optional<u32> Translator::getKnownValue(u32 pc, u32 reg) const {
    const auto found = mValues.find(pc);
    if (found == mValues.end()) {
        return std::nullopt;
    }
    return found->second.getRegister(reg);
}

std::string Translator::emitBase(u32 pc, u32 reg) const {
    if (const std::optional<u32> value = getKnownValue(pc, reg)) {
        return std::format("FixedAddress{{0x{:08x}u}}", *value);
    }
    return std::string(regName(reg));
}

std::optional<Resolved> Translator::resolveAccess(u32 pc, u32 base, s32 offset) const {
    const auto found = mValues.find(pc);
    if (found == mValues.end()) {
        return std::nullopt;
    }
    const std::optional<u32> value = found->second.getRegister(base);
    if (!value) {
        return std::nullopt;
    }
    return resolveWindow(*value + static_cast<u32>(offset));
}

std::optional<std::string> Translator::emitInstruction(u32 pc, Instruction insn) const {
    // A nop is not a dead write. It writes nothing, but the units behind the
    // instruction stream still move on for it.
    if (insn.word != 0 && mDeadWrites.count(pc) != 0) {
        return std::string{};
    }
    const auto found = mValues.find(pc);
    if (found != mValues.end()) {
        const std::optional<KnownWrite> write = foldConstant(found->second, getOp(insn), insn);
        if (write) {
            return writeConstant(write->reg, write->value);
        }
    }
    return emitOne(pc, insn);
}

std::string Translator::getParameters() const {
    return std::format("{}& m, UEE_MAYBE_UNUSED {}& {}, UEE_MAYBE_UNUSED Memory& mem",
                       getMachineType(), getContextType(), getStateName());
}

std::string Translator::getParameterTypes() const {
    return std::format("{}&, {}&, Memory&", getMachineType(), getContextType());
}

std::string Translator::getArguments() const {
    return std::format("m, {}, mem", getStateName());
}

std::string Translator::go(u32 target, u32 function) const {
    if (mCode.owns(function, target)) {
        return std::format("goto L_{:08x};", target);
    }
    // A jump, so the target holds this function's return address and answer.
    if (mCode.countIfReturnOnly(target)) {
        return std::format("return leave({0}, {1}, returnTo);", getStateName(), readGpr(reg::ra));
    }
    if (mCode.hasBody(target)) {
        noteCall(function, target);
        return std::format("return {}({}, 0x{:08x}u);", nameOf(target), getArguments(), target);
    }
    return std::format("return leave({}, 0x{:08x}u, returnTo);", getStateName(), target);
}

// A block that ends by jumping to the one written next can just run into it.
void Translator::dropJumpToNext(std::string& text, u32 next) {
    const std::string only = std::format("    goto L_{:08x};\n", next);
    if (text == only) {
        text.clear();
        return;
    }
    const std::string tail = "\n" + only;
    if (text.size() > tail.size() &&
        text.compare(text.size() - tail.size(), tail.size(), tail) == 0) {
        text.resize(text.size() - tail.size() + 1);
    }
}

std::string Translator::annotate(const std::string& statement, u32 pc, Instruction insn,
                            s32 depth) {
    const std::string line = std::string(depth * 4, ' ') + statement;
    const usize column = std::max(line.size() + 1, static_cast<usize>(kCommentColumn));
    return std::format("{:<{}}// {:08x}  {}", line, column, pc, disassemble(insn, pc));
}

// Not landing on the next instruction means it never completed.
std::string Translator::stepAt(u32 pc, s32 depth) const {
    const std::string indent(4 * static_cast<usize>(depth), ' ');
    return std::format("{0}interpreterFallback(m, 0x{2:08x}u);\n{0}if ({1}.cpu.pc != 0x{3:08x}u) {{\n"
                       "{0}    return leave({1}, {1}.cpu.pc, returnTo);\n{0}}}\n",
                       indent, getStateName(), pc, pc + 4);
}

std::string Translator::emitBeforeInstructionAt(s32 depth) const {
    const std::string statement = emitBeforeInstruction();
    if (statement.empty()) {
        return {};
    }
    return std::format("{}{}\n", std::string(4 * static_cast<usize>(depth), ' '), statement);
}

// The slot resolves the pc itself, so a target picked before it is stale.
bool Translator::delaySlotTransfers(u32 pc) {
    const std::optional<Instruction> delayed = fetch(pc + 4);
    return delayed && getControlFlow(getOp(*delayed), *delayed, pc + 4).flow != Flow::Next;
}

std::string Translator::emitTransferringSlot(const std::string& pending, u32 pc, s32 depth) const {
    const std::string indent(4 * static_cast<usize>(depth), ' ');
    return std::format("{0}{1}.cpu.pc = {2};\n{0}interpreterFallbackDelaySlot(m, 0x{3:08x}u);\n"
                       "{0}return leave({1}, {1}.cpu.pc, returnTo);\n",
                       indent, getStateName(), pending, pc + 4);
}

// A block at a time, so the totals agree with the interpreter at every boundary.
std::string Translator::countedAt(u32 function, u32 blockStart,
                                  const std::string& body) const {
    if (mInlineInstructions == 0) {
        return body;
    }
    // Only a block that can be arrived at twice has to ask whether the run is
    // over; the rest owe the clock a count and nothing more.
    const bool comesRoundAgain =
        mCode.isLoopHeader(blockStart) ||
        (blockStart == function && !mCode.runsToCompletion(function));
    if (!comesRoundAgain) {
        const std::string counted = countInstructionsOnly(mInlineInstructions);
        return counted.empty() ? body : std::format("    {}\n", counted) + body;
    }
    const std::string call = countInstructions(mInlineInstructions, blockStart);
    if (call.empty()) {
        return body;
    }
    // The block has not run, and its first instruction is where it resumes.
    return std::format("    if (!{}) return {}(m, 0x{:08x}u);\n", call, getStopFunction(),
                       blockStart) +
           body;
}

std::string Translator::emitBlock(u32 blockStart, u32 function) {
    std::string out;
    const auto entry = mBlockEntries.find(blockStart);
    const BlockValues& start = entry == mBlockEntries.end() ? kNothingKnown : entry->second;
    mValues = analyseBlock(mMemory, mCode, blockStart, start);
    mDeadWrites = mDeadWritesByBlock[blockStart];
    mInlineInstructions = 0;
    u32 pc = blockStart;
    for (;;) {
        const std::optional<Instruction> insn = fetch(pc);
        if (!insn) {
            return countedAt(function, blockStart, out + std::format("    return leave({}, 0x{:08x}u, returnTo);\n",
                                            getStateName(), pc));
        }
        const Op op = getOp(*insn);
        const ControlFlow cf = getControlFlow(op, *insn, pc);

        if (cf.flow == Flow::Next || cf.flow == Flow::Trap) {
            {
                const auto values = mValues.find(pc);
                const std::optional<Trap> trap = emitTrap(
                    *insn, values == mValues.end() ? kNothingKnown : values->second, pc);
                if (trap) {
                    out += emitBeforeInstructionAt(1);
                    out += annotate(trap->statement, pc, *insn, 1) + "\n";
                    ++mInlineInstructions;
                    if (!trap->continues) {
                        return countedAt(function, blockStart, out + std::format("    return leave({0}, {0}.cpu.pc, "
                                                         "returnTo);\n", getStateName()));
                    }
                    pc += 4;
                    if (mCode.isBlockStart(pc)) {
                        return countedAt(function, blockStart, out + std::format("    {}\n", go(pc, function)));
                    }
                    continue;
                }
            }
            if (const std::optional<std::string> statement = emitInstruction(pc, *insn)) {
                ++mInlineInstructions;
                out += emitBeforeInstructionAt(1);
                if (!statement->empty()) {
                    out += annotate(*statement, pc, *insn, 1) + "\n";
                }
            } else {
                out += annotate(std::format("interpreterFallback(m, 0x{:08x}u);", pc), pc, *insn, 1) + "\n";
                out += std::format("    if ({0}.cpu.pc != 0x{1:08x}u) {{\n"
                                   "        return leave({0}, {0}.cpu.pc, returnTo);\n    }}\n",
                                   getStateName(), pc + 4);
            }
            pc += 4;
            if (mCode.isBlockStart(pc)) {
                return countedAt(function, blockStart, out + std::format("    {}\n", go(pc, function)));
            }
            continue;
        }
        if (cf.flow == Flow::Trap) {
            out += annotate(std::format("interpreterFallback(m, 0x{:08x}u);", pc), pc, *insn, 1) + "\n";
            out += std::format("    call(m, {0}.cpu.pc, {1});\n    return leave({0}, "
                               "{0}.cpu.pc, returnTo);\n",
                               getStateName(), readGpr(reg::ra));
            return countedAt(function, blockStart, out);
        }
        return countedAt(function, blockStart, out + emitExit(cf, op, *insn, pc, function));
    }
}

// Both orders matter: a program may name the same register twice.
std::string Translator::emitExit(const ControlFlow& cf, Op op, Instruction insn, u32 pc,
                                 u32 function) {
    const std::optional<Instruction> delayed = fetch(pc + 4);
    const std::optional<std::string> inlineSlot =
        delayed ? emitInstruction(pc + 4, *delayed) : std::nullopt;
    const auto slotAt = [&](s32 depth) {
        if (!inlineSlot) {
            return stepAt(pc + 4, depth);
        }
        if (inlineSlot->empty()) {
            return emitBeforeInstructionAt(depth);
        }
        return emitBeforeInstructionAt(depth) +
               annotate(*inlineSlot, pc + 4, *delayed, depth) + "  (delay slot)\n";
    };
    const bool slotTransfers = delaySlotTransfers(pc);

    // A likely branch nullifies its slot at run time, so count it where it runs.
    ++mInlineInstructions;
    const bool slotAlwaysRuns = cf.flow != Flow::BranchLikely;
    if (inlineSlot && slotAlwaysRuns) {
        ++mInlineInstructions;
    }
    const std::string slotCount =
        inlineSlot && !slotAlwaysRuns ? countInstructions(1, pc) : std::string{};
    const std::string countSlot =
        slotCount.empty() ? std::string{} : std::format("            {};\n", slotCount);

    const auto linkAt = [&](s32 depth) {
        return cf.link == 0 ? std::string{}
                            : std::format("{}{}\n",
                                          std::string(4 * static_cast<usize>(depth), ' '),
                                          writeGpr(cf.link, pc + 8));
    };

    std::string out = emitBeforeInstructionAt(1);

    // A function that never writes ra goes back where it was called from.
    const bool returnsPlainly =
        cf.flow == Flow::JumpRegister && cf.link == 0 && cf.source == reg::ra &&
        mKeepsReturnAddress && !slotTransfers;

    if (cf.flow == Flow::JumpRegister) {
        out += "    {\n";
        out += annotate(returnsPlainly ? std::string{}
                                       : std::format("const u32 destination = {};",
                                                     readGpr(cf.source)),
                        pc, insn, 2) + "\n";
        out += linkAt(2);
        if (slotTransfers) {
            return out + emitTransferringSlot("destination", pc, 2) + "    }\n";
        }
        out += slotAt(2);
        if (cf.link != 0) {
            out += std::format("        {}.cpu.pc = destination;\n", getStateName());
            out += std::format("        if (!call(m, destination, 0x{:08x}u)) return false;\n"
                               "    }}\n", pc + 8);
            out += std::format("    {}\n", go(pc + 8, function));
        } else if (cf.source == reg::ra) {
            out += returnsPlainly
                       ? std::format("        {}.cpu.pc = returnTo;\n        return true;\n    }}\n",
                                     getStateName())
                       : std::format("        return leave({}, destination, returnTo);\n    }}\n",
                                     getStateName());
        } else {
            out += std::format("        {}.cpu.pc = destination;\n", getStateName());
            const std::vector<u32>* table = mCode.getJumpTable(pc);
            if (table != nullptr && !table->empty()) {
                out += "        switch (destination) {\n";
                std::set<u32> seen;
                for (u32 target : *table) {
                    if (!seen.insert(target).second) {
                        continue;
                    }
                    out += std::format("            case 0x{:08x}u: {}\n", target,
                                       go(target, function));
                }
                out += "            default: break;\n        }\n";
            }
            out += std::format("        return leave({}, destination, returnTo);\n    }}\n",
                               getStateName());
        }
        return out;
    }

    if (cf.flow == Flow::Jump) {
        if (slotTransfers && cf.link == 0) {
            return out + emitTransferringSlot(std::format("0x{:08x}u", cf.target), pc, 1);
        }
        out += linkAt(1);
        out += slotAt(1);
        if (cf.link == 0) {
            out += annotate(go(cf.target, function), pc, insn, 1) + "\n";
            return out;
        }
        if (const std::optional<u32> returnOnly = mCode.countIfReturnOnly(cf.target)) {
            mInlineInstructions += *returnOnly;
            out += annotate(std::string{}, pc, insn, 1) + "\n";
        } else if (const std::optional<std::string> inlined =
                       emitInlinedCall(cf.target, pc + 8)) {
            out += annotate(*inlined, pc, insn, 1) + "\n";
        } else if (mCode.isFunction(cf.target) && mCode.hasBody(cf.target)) {
            noteCall(function, cf.target);
            out += annotate(std::format("if (!{}({}, 0x{:08x}u)) return false;",
                                        nameOf(cf.target), getArguments(), cf.target),
                            pc, insn, 1) + "\n";
        } else {
            out += annotate(std::format("if (!call(m, 0x{:08x}u, 0x{:08x}u)) return false;",
                                        cf.target, pc + 8), pc, insn, 1) + "\n";
        }
        out += std::format("    {}\n", go(pc + 8, function));
        return out;
    }

    if (alwaysBranches(op, insn)) {
        if (slotTransfers) {
            return out + emitTransferringSlot(std::format("0x{:08x}u", cf.target), pc, 1);
        }
        out += slotAt(1);
        out += annotate(go(cf.target, function), pc, insn, 1) + "\n";
        return out;
    }

    const std::optional<std::string> condition = emitBranchCondition(op, insn);
    out += "    {\n";
    out += annotate(std::format("const bool taken = {};", condition ? *condition : "false"),
                    pc, insn, 2) + "\n";
    out += linkAt(2);
    if (cf.flow == Flow::BranchLikely) {
        out += "        if (taken) {\n";
        if (slotTransfers) {
            out += emitTransferringSlot(std::format("0x{:08x}u", cf.target), pc, 3);
            out += std::format("        }}\n    }}\n    {}\n", go(pc + 8, function));
            return out;
        }
        out += countSlot;
        out += slotAt(3);
        out += std::format("            {}\n        }}\n    }}\n", go(cf.target, function));
        out += std::format("    {}\n", go(pc + 8, function));
        return out;
    }
    if (slotTransfers) {
        out += emitTransferringSlot(
            std::format("taken ? 0x{:08x}u : 0x{:08x}u", cf.target, pc + 8), pc, 2);
        return out + "    }\n";
    }
    out += slotAt(2);
    out += std::format("        if (taken) {{\n            {}\n        }}\n    }}\n",
                       go(cf.target, function));
    out += std::format("    {}\n", go(pc + 8, function));
    return out;
}

std::string Translator::emitFunction(u32 function, const std::vector<u32>& blockStarts) {
    std::vector<u32> order{function};
    for (u32 blockStart : blockStarts) {
        if (blockStart != function) {
            order.push_back(blockStart);
        }
    }
    mBlockEntries =
        analyseFunction(mMemory, mCode, function, order, mProgramValues, &mClobbered);
    const std::string reentry = emitReentry(function, order);
    mKeepsReturnAddress = reentry.empty() && keepsReturnAddress(mMemory, mCode, function, order);
    mDeadWritesByBlock =
        findDeadWritesInFunction(mMemory, mCode, order, mBlockEntries, getLoadDelay());
    std::map<u32, std::string> blocks;
    for (u32 blockStart : order) {
        blocks[blockStart] = emitBlock(blockStart, function);
    }
    for (usize i = 0; i + 1 < order.size(); ++i) {
        dropJumpToNext(blocks[order[i]], order[i + 1]);
    }
    std::string all;
    for (u32 blockStart : order) {
        all += blocks[blockStart];
    }
    std::string body;
    for (u32 blockStart : order) {
        // The first block is reached by falling past the switch.
        const bool named = blockStart != function && mCode.isResumePoint(blockStart);
        if (named || all.find(std::format("goto L_{:08x};", blockStart)) != std::string::npos) {
            body += std::format("\nL_{:08x}:\n", blockStart);
        } else {
            body += "\n";
        }
        body += blocks[blockStart];
    }

    const std::optional<u64> hash =
        hashInstructions(mMemory, function, mCode.getFunctionEnd(function));
    if (hash && std::getenv("UEE_DUMP_HASHES") != nullptr) {
        std::fprintf(stderr, "HASH %016llx %s\n", static_cast<unsigned long long>(*hash),
                     nameOf(function).c_str());
    }
    if (const Replacement* replacement = findReplacement(hash)) {
        // Or a deadline the guest schedules around drifts every time this runs.
        const u32 instructions = (mCode.getFunctionEnd(function) - function) / 4;
        // Written like any other return: whoever dispatched this cannot know
        // where it went unless it says so.
        return std::format("\n// {}\nbool {}({}, u32) {{\n"
                           "    const u32 returnTo = {};\n    countOnly(m, {});\n    {}\n"
                           "    {}.cpu.pc = returnTo;\n    return true;\n}}\n",
                           replacement->name, nameOf(function), getParameters(),
                           readGpr(reg::ra), instructions, replacement->statement,
                           getStateName());
    }

    const std::string entry = reentry.empty() ? "" : " entry";
    std::string out = std::format("\nbool {}({}, u32{}) {{\n", nameOf(function),
                                  getParameters(), entry);
    // Meaningless in a function picked up part way through, which had no caller.
    out += reentry.empty()
               ? std::format("    UEE_MAYBE_UNUSED const u32 returnTo = {};\n", readGpr(reg::ra))
               : std::format("    UEE_MAYBE_UNUSED const u32 returnTo =\n"
                             "        entry == 0x{:08x}u ? {} : kResumedNotCalled;\n",
                             function, readGpr(reg::ra));
    out += emitHookCheck(function);
    out += reentry;
    out += body;
    out += "}\n";
    return out;
}

// The run loop resumes wherever the guest left off, which after a syscall or a
// thread switch is rarely a function's first instruction.
std::string Translator::emitReentry(u32 function, const std::vector<u32>& order) const {
    std::string cases;
    for (u32 blockStart : order) {
        if (blockStart != function && mCode.isResumePoint(blockStart)) {
            cases += std::format("            case 0x{0:08x}u: goto L_{0:08x};\n", blockStart);
        }
    }
    if (cases.empty()) {
        return {};
    }
    return std::format("    if (entry != 0x{:08x}u) {{\n        switch (entry) {{\n", function) +
           cases + "            default: break;\n        }\n    }\n";
}

// Trimming these to what each unit calls is worth about four percent.
std::string Translator::emitDeclarations() const {
    std::string out = std::format("#pragma once\n\n#include \"{}\"\n#include \"{}\"\n\n",
                                  getHandlersHeader(), getRuntimeHeader());
    out += "namespace uee {\nusing namespace reg;\n";
    out += emitBeforeNamespace();
    out += emitNamespaceOpen();
    for (const auto& [function, blockStarts] : mCode.getBlocksByFunction()) {
        out += std::format("\nbool {}({}, u32);", nameOf(function), getParameterTypes());
    }
    out += "\n";
    out += emitNamespaceClose();
    out += "\n}  // namespace uee\n";
    return out;
}

std::string Translator::emitDispatch() const {
    // The addresses are data, not control flow the optimiser has to chew through.
    std::string out = std::format("\nstruct Entry {{\n    u32 address;\n"
                                  "    bool (*run)({}, u32);\n}};\n", getParameterTypes());
    // Address order for the binary search, and only real entry addresses.
    std::map<u32, u32> owners;
    for (const auto& [function, blockStarts] : mCode.getBlocksByFunction()) {
        owners[function] = function;
        for (u32 blockStart : blockStarts) {
            if (mCode.isResumePoint(blockStart)) {
                owners[blockStart] = function;
            }
        }
    }
    out += "\nconstexpr Entry kEntries[] = {\n";
    for (const auto& [blockStart, function] : owners) {
        out += std::format("    {{0x{:08x}u, {}}},\n", blockStart, nameOf(function));
    }
    out += "};\n";

    // Built from kEntries rather than written out, which would be a million
    // mostly empty words of source.
    out += "\nconstexpr u32 kEntryWords =\n"
           "    (kEntries[std::size(kEntries) - 1].address - kEntries[0].address) / 4 + 1;\n";
    out += "\nstd::vector<u32> indexEntriesByAddress() {\n";
    out += "    std::vector<u32> byAddress(kEntryWords, 0);\n";
    out += "    for (u32 i = 0; i < std::size(kEntries); ++i) {\n";
    out += "        byAddress[(kEntries[i].address - kEntries[0].address) / 4] = i + 1;\n";
    out += "    }\n    return byAddress;\n}\n";

    out += std::format("\nbool runFunction({}& m, u32 pc) {{\n", getMachineType());
    out += "    static const std::vector<u32> byAddress = indexEntriesByAddress();\n";
    out += "    const u32 word = (pc - kEntries[0].address) / 4;\n";
    out += "    if (pc % 4 != 0 || word >= kEntryWords || byAddress[word] == 0) {\n";
    out += "        return false;\n";
    out += "    }\n";
    out += "    const Entry* found = &kEntries[byAddress[word] - 1];\n";
    out += std::format("    {0}& {1} = m.{1}();\n", getContextType(), getStateName());
    out += "    Memory& mem = m.memory;\n";
    // The function writes where it went, so nothing here has to guess it from
    // a register that only means anything at a real entry.
    out += std::format("    found->run({}, pc);\n", getArguments());
    out += "    return true;\n";
    out += "}\n";
    return out;
}

// Address order, so functions that run together stay together.
std::string Translator::reportUnhookedRenderPaths() const {
    std::map<u32, u32> functionOf;
    for (const auto& [function, blockStarts] : mCode.getBlocksByFunction()) {
        functionOf[function] = function;
    }
    const auto ownerOf = [&](u32 pc) {
        const auto after = functionOf.upper_bound(pc);
        return after == functionOf.begin() ? 0 : std::prev(after)->second;
    };

    std::set<u32> frontier;
    for (const u32 site : mRenderKickSites) {
        if (const u32 owner = ownerOf(site); owner != 0) {
            frontier.insert(owner);
        }
    }
    const usize kickers = frontier.size();

    std::set<u32> seen = frontier;
    std::set<u32> roots;
    while (!frontier.empty()) {
        std::set<u32> next;
        for (const u32 callee : frontier) {
            const auto callers = mCallers.find(callee);
            if (callers == mCallers.end() || callers->second.empty()) {
                roots.insert(callee);
                continue;
            }
            for (const u32 caller : callers->second) {
                if (mHookable.count(caller) != 0 || !seen.insert(caller).second) {
                    continue;
                }
                next.insert(caller);
            }
        }
        frontier = std::move(next);
    }
    for (const u32 root : roots) {
        seen.erase(root);
    }

    std::string out = std::format("{} functions kick VIF1 or the GIF; {} entry points reach one "
                                  "with no hook in the way:\n", kickers, roots.size());
    for (const u32 root : roots) {
        out += std::format("    {}\n", nameOf(root));
    }
    return out;
}

TranslatedSource Translator::render(usize units, std::string_view declarationsHeader) {
    std::vector<std::string> bodies;
    bodies.reserve(mCode.getBlocksByFunction().size());
    usize total = 0;
    for (const auto& [function, blockStarts] : mCode.getBlocksByFunction()) {
        bodies.push_back(emitFunction(function, blockStarts));
        total += bodies.back().size();
    }

    TranslatedSource out;
    out.declarations = emitDeclarations();
    if (!mHookable.empty()) {
        log.info("{}", reportUnhookedRenderPaths());
    }

    const auto open = [&] {
        std::string unit = std::format("#include \"{}\"\n\n", declarationsHeader);
        unit += "#include <algorithm>\n#include <vector>\n\nnamespace uee {\n";
        unit += emitNamespaceOpen();
        return unit;
    };
    const auto close = [&](std::string& unit) {
        unit += emitNamespaceClose();
        unit += "\n}  // namespace uee\n";
    };

    // The dispatch table and whatever defines the module go in the first unit,
    // which is why its share of the functions is what is left after them.
    std::string unit = open();
    unit += emitDispatch();
    unit += emitDefinitions();
    // On a whole game the table is a unit's worth of work by itself.
    const usize dispatch = unit.size();
    const usize share = total / std::max<usize>(units, 1);
    usize forFunctions = units;
    if (dispatch > share && units > 1) {
        close(unit);
        out.units.push_back(std::move(unit));
        unit = open();
        --forFunctions;
    }

    // Cumulative, or the drift leaves the last units empty.
    usize written = 0;
    usize index = 0;
    for (std::string& body : bodies) {
        if (written >= total * (index + 1) / forFunctions && out.units.size() + 1 < units) {
            close(unit);
            out.units.push_back(std::move(unit));
            unit = open();
            ++index;
        }
        written += body.size();
        unit += body;
    }
    close(unit);
    out.units.push_back(std::move(unit));
    return out;
}

}  // namespace uee
