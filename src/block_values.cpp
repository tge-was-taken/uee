#include "block_values.h"

#include "code_map.h"

#include <utility>

namespace uee {

std::optional<u32> BlockValues::getRegister(u32 reg) const {
    if ((mKnown & (1u << reg)) == 0) {
        return std::nullopt;
    }
    return mValues[reg];
}

std::optional<u32> BlockValues::getAddress(Instruction insn) const {
    const std::optional<u32> base = getRegister(insn.rs());
    if (!base) {
        return std::nullopt;
    }
    return *base + static_cast<u32>(insn.simm());
}

void BlockValues::setRegister(u32 reg, u32 value) {
    if (reg == 0) {
        return;
    }
    mValues[reg] = value;
    mKnown |= 1u << reg;
    mBased &= ~(1u << reg);
}

void BlockValues::setBase(u32 reg, u32 base) {
    if (reg == 0) {
        return;
    }
    mBases[reg] = base;
    mBased |= 1u << reg;
    mKnown &= ~(1u << reg);
}

std::optional<u32> ReadOnlyImage::read32(u32 address) const {
    for (const auto& [from, to] : mRanges) {
        if (address >= from && address + 4 <= to) {
            return mMemory->peek32(address);
        }
    }
    return std::nullopt;
}

std::optional<u32> BlockValues::getBase(u32 reg) const {
    if ((mKnown >> reg) & 1u) {
        return mValues[reg];
    }
    return ((mBased >> reg) & 1u) ? std::optional(mBases[reg]) : std::nullopt;
}

std::optional<u32> BlockValues::getAddressBase(Instruction insn) const {
    const std::optional<u32> base = getBase(insn.rs());
    if (!base) {
        return std::nullopt;
    }
    return *base + static_cast<u32>(insn.simm());
}

void BlockValues::forget(u32 reg) {
    mKnown &= ~(1u << reg);
    mKnown |= 1u;
    mBased &= ~(1u << reg);
}

void BlockValues::forgetAll() {
    mKnown = 1u;
}

bool BlockValues::meet(const BlockValues& other) {
    const u32 before = mKnown;
    const u32 basedBefore = mBased;
    for (u32 reg = 1; reg < 32; ++reg) {
        const u32 mask = 1u << reg;
        if ((mKnown & mask) != 0 &&
            ((other.mKnown & mask) == 0 || other.mValues[reg] != mValues[reg])) {
            mKnown &= ~mask;
        }
    }
    for (u32 reg = 1; reg < 32; ++reg) {
        const u32 mask = 1u << reg;
        if ((mBased & mask) != 0 &&
            (other.getBase(reg) != std::optional(mBases[reg]))) {
            mBased &= ~mask;
        }
    }
    if (mLoadedPreviously != other.mLoadedPreviously) {
        // Neither block may assume the other's load has landed.
        forget(mLoadedPreviously);
        forget(other.mLoadedPreviously == kNoRegister ? 0 : other.mLoadedPreviously);
        mLoadedPreviously = kNoRegister;
    }
    return mKnown != before || mBased != basedBefore;
}

namespace {

// modelled says which register is known, value that the contents are too.
struct Folded {
    bool modelled = false;
    u32 destination = 0;
    std::optional<u32> value;
};

// Only results that are the sign extension of a low word are folded, so a
// folded value always goes back with a 32-bit write.
std::optional<u32> carriedBase(const BlockValues& values, Op op, Instruction insn) {
    switch (op) {
        case Op::add: case Op::addu: case Op::sub: case Op::subu:
        case Op::dadd: case Op::daddu: case Op::dsub: case Op::dsubu:
        case Op::or_: {
            const std::optional<u32> left = values.getBase(insn.rs());
            const std::optional<u32> right = values.getBase(insn.rt());
            if (left && right) {
                return std::nullopt;
            }
            return left ? left : right;
        }
        case Op::addi: case Op::addiu: case Op::daddi: case Op::daddiu: {
            const std::optional<u32> base = values.getBase(insn.rs());
            return base ? std::optional(*base + static_cast<u32>(insn.simm())) : std::nullopt;
        }
        case Op::ori: {
            const std::optional<u32> base = values.getBase(insn.rs());
            return base ? std::optional(*base | insn.imm()) : std::nullopt;
        }
        default:
            return std::nullopt;
    }
}

Folded fold(const BlockValues& values, Op op, Instruction insn) {
    const u32 rt = insn.rt();
    const u32 rd = insn.rd();
    const std::optional<u32> a = values.getRegister(insn.rs());
    const std::optional<u32> b = values.getRegister(rt);
    const auto withImmediate = [&](auto combine) {
        return Folded{true, rt, a ? std::optional(combine(*a)) : std::nullopt};
    };
    const auto both = [&](auto combine) {
        return Folded{true, rd, a && b ? std::optional(combine(*a, *b)) : std::nullopt};
    };
    const auto shifted = [&](auto combine) {
        return Folded{true, rd, b ? std::optional(combine(*b, insn.sa())) : std::nullopt};
    };
    const auto shiftedBy = [&](auto combine) {
        return Folded{true, rd, a && b ? std::optional(combine(*b, *a & 31u)) : std::nullopt};
    };

    switch (op) {
        case Op::lui: return Folded{true, rt, insn.imm() << 16};

        case Op::addi:
        case Op::addiu:
            return withImmediate([&](u32 x) { return x + static_cast<u32>(insn.simm()); });
        case Op::andi: return withImmediate([&](u32 x) { return x & insn.imm(); });
        case Op::ori: return withImmediate([&](u32 x) { return x | insn.imm(); });
        case Op::xori: return withImmediate([&](u32 x) { return x ^ insn.imm(); });
        case Op::slti:
            return withImmediate(
                [&](u32 x) { return static_cast<s32>(x) < insn.simm() ? 1u : 0u; });
        case Op::sltiu:
            return withImmediate(
                [&](u32 x) { return x < static_cast<u32>(insn.simm()) ? 1u : 0u; });

        case Op::add:
        case Op::addu: return both([](u32 x, u32 y) { return x + y; });
        case Op::sub:
        case Op::subu: return both([](u32 x, u32 y) { return x - y; });
        case Op::and_: return both([](u32 x, u32 y) { return x & y; });
        case Op::or_: return both([](u32 x, u32 y) { return x | y; });
        case Op::xor_: return both([](u32 x, u32 y) { return x ^ y; });
        case Op::nor: return both([](u32 x, u32 y) { return ~(x | y); });
        case Op::slt:
            return both([](u32 x, u32 y) {
                return static_cast<s32>(x) < static_cast<s32>(y) ? 1u : 0u;
            });
        case Op::sltu: return both([](u32 x, u32 y) { return x < y ? 1u : 0u; });

        case Op::sll: return shifted([](u32 x, u32 by) { return x << by; });
        case Op::srl: return shifted([](u32 x, u32 by) { return x >> by; });
        case Op::sra:
            return shifted(
                [](u32 x, u32 by) { return static_cast<u32>(static_cast<s32>(x) >> by); });
        case Op::sllv: return shiftedBy([](u32 x, u32 by) { return x << by; });
        case Op::srlv: return shiftedBy([](u32 x, u32 by) { return x >> by; });
        case Op::srav:
            return shiftedBy(
                [](u32 x, u32 by) { return static_cast<u32>(static_cast<s32>(x) >> by); });

        default: return {};
    }
}

bool writesNoRegister(Op op) {
    switch (op) {
        case Op::sb: case Op::sh: case Op::sw: case Op::sd: case Op::sq:
        case Op::swl: case Op::swr: case Op::sdl: case Op::sdr:
        case Op::swc1: case Op::sqc2: case Op::lwc1: case Op::lqc2:
        case Op::beq: case Op::bne: case Op::blez: case Op::bgtz:
        case Op::beql: case Op::bnel: case Op::blezl: case Op::bgtzl:
        case Op::bltz: case Op::bgez: case Op::bltzl: case Op::bgezl:
        case Op::bc1t: case Op::bc1f: case Op::bc1tl: case Op::bc1fl:
        case Op::bc0t: case Op::bc0f: case Op::bc0tl: case Op::bc0fl:
        case Op::j: case Op::jr:
        case Op::mtc0: case Op::mtc1: case Op::ctc1: case Op::qmtc2_ni: case Op::ctc2_ni:
        case Op::mthi: case Op::mtlo: case Op::mthi1: case Op::mtlo1:
        case Op::mtsa: case Op::mtsab: case Op::mtsah:
        case Op::pmthi: case Op::pmtlo: case Op::pmthl_lw:
        case Op::div: case Op::divu: case Op::div1: case Op::divu1:
        case Op::pdivw: case Op::pdivuw: case Op::pdivbw:
        case Op::di: case Op::ei:
        case Op::cache: case Op::pref: case Op::sync_p:
        case Op::tlbwi: case Op::tlbwr: case Op::tlbr: case Op::tlbp:
        case Op::teq: case Op::tge: case Op::tgeu: case Op::tlt: case Op::tltu: case Op::tne:
            return true;
        default:
            return false;
    }
}

bool writesCoprocessorOnly(Op op) {
    switch (op) {
        case Op::add_s: case Op::sub_s: case Op::mul_s: case Op::div_s:
        case Op::madd_s: case Op::msub_s: case Op::max_s: case Op::min_s:
        case Op::rsqrt_s: case Op::sqrt_s: case Op::abs_s: case Op::neg_s: case Op::mov_s:
        case Op::adda_s: case Op::suba_s: case Op::mula_s: case Op::madda_s: case Op::msuba_s:
        case Op::cvt_w_s: case Op::cvt_s_w:
        case Op::c_f_s: case Op::c_eq_s: case Op::c_lt_s: case Op::c_le_s:
            return true;
        default:
            return false;
    }
}

bool writesTargetOnly(Op op) {
    switch (op) {
        case Op::lb: case Op::lbu: case Op::lh: case Op::lhu:
        case Op::lw: case Op::lwu: case Op::ld: case Op::lq:
        case Op::lwl: case Op::lwr: case Op::ldl: case Op::ldr:
        case Op::mfc0: case Op::mfc1: case Op::cfc1: case Op::qmfc2_ni: case Op::cfc2_ni:
        case Op::daddi: case Op::daddiu:
            return true;
        default:
            return false;
    }
}

// A doubleword result is not the sign extension of its low word.
bool writesDestinationOnly(Op op) {
    switch (op) {
        case Op::movz: case Op::movn:
        case Op::dadd: case Op::daddu: case Op::dsub: case Op::dsubu:
        case Op::dsll: case Op::dsrl: case Op::dsra:
        case Op::dsll32: case Op::dsrl32: case Op::dsra32:
        case Op::dsllv: case Op::dsrlv: case Op::dsrav:
        case Op::mult: case Op::multu: case Op::mult1: case Op::multu1:
        case Op::madd: case Op::madd1: case Op::maddu: case Op::maddu1:
        case Op::mfhi: case Op::mflo: case Op::mfhi1: case Op::mflo1: case Op::mfsa:
        case Op::pmfhi: case Op::pmflo:
        case Op::pmfhl_lw: case Op::pmfhl_uw: case Op::pmfhl_slw:
        case Op::pmfhl_lh: case Op::pmfhl_sh:
            return true;
        default:
            return false;
    }
}

bool isLoad(Op op) {
    switch (op) {
        case Op::lb: case Op::lbu: case Op::lh: case Op::lhu:
        case Op::lw: case Op::lwu: case Op::ld: case Op::lq:
        case Op::lwl: case Op::lwr: case Op::ldl: case Op::ldr:
            return true;
        default:
            return false;
    }
}

// Emitted with the address instead of the register, so whatever computed it goes.
bool takesResolvedBase(Op op) {
    switch (op) {
        case Op::lb: case Op::lbu: case Op::lh: case Op::lhu:
        case Op::lw: case Op::lwu: case Op::ld: case Op::lq:
        case Op::lwl: case Op::lwr: case Op::ldl: case Op::ldr:
        case Op::sb: case Op::sh: case Op::sw: case Op::sd: case Op::sq:
        case Op::swl: case Op::swr: case Op::sdl: case Op::sdr:
        case Op::lwc1: case Op::swc1: case Op::lqc2: case Op::sqc2:
            return true;
        default:
            return false;
    }
}

bool linksToReturnAddress(Op op) {
    switch (op) {
        case Op::jal: case Op::jalr:
        case Op::bltzal: case Op::bgezal: case Op::bltzall: case Op::bgezall:
            return true;
        default:
            return false;
    }
}


// Complete says both sets are exact; an unrecognised instruction touches all.
struct RegisterUse {
    bool complete = false;
    std::optional<u32> writes;
    // A 32-bit or 64-bit write leaves the upper half, so it kills no wide write.
    bool writesWholeRegister = false;
    u32 reads = 0;
};

// The kernel never writes it.
constexpr u32 kSyscallNumberRegister = 3;

constexpr u32 bit(u32 reg) {
    return reg == 0 ? 0u : (1u << reg);
}

RegisterUse useOf(Op op, Instruction insn) {
    const u32 rs = insn.rs();
    const u32 rt = insn.rt();
    const u32 rd = insn.rd();

    switch (op) {
        case Op::lui: return {true, rt, false, 0};

        case Op::addi: case Op::addiu: case Op::andi: case Op::ori: case Op::xori:
        case Op::slti: case Op::sltiu: case Op::daddi: case Op::daddiu:
            return {true, rt, false, bit(rs)};

        case Op::add: case Op::addu: case Op::sub: case Op::subu:
        case Op::and_: case Op::or_: case Op::xor_: case Op::nor:
        case Op::slt: case Op::sltu:
        case Op::dadd: case Op::daddu: case Op::dsub: case Op::dsubu:
        case Op::sllv: case Op::srlv: case Op::srav:
        case Op::dsllv: case Op::dsrlv: case Op::dsrav:
            return {true, rd, false, bit(rs) | bit(rt)};

        case Op::sll: case Op::srl: case Op::sra:
        case Op::dsll: case Op::dsrl: case Op::dsra:
        case Op::dsll32: case Op::dsrl32: case Op::dsra32:
            return {true, rd, false, bit(rt)};

        // Both leave rd alone unless the condition holds, so rd is read too.
        case Op::movz: case Op::movn:
            return {true, rd, false, bit(rs) | bit(rt) | bit(rd)};

        case Op::lb: case Op::lbu: case Op::lh: case Op::lhu:
        case Op::lw: case Op::lwu: case Op::ld:
            return {true, rt, false, bit(rs)};
        case Op::lq: return {true, rt, true, bit(rs)};
        // An unaligned pair merges into whatever the register already held.
        case Op::lwl: case Op::lwr: case Op::ldl: case Op::ldr:
            return {true, rt, false, bit(rs) | bit(rt)};

        case Op::sb: case Op::sh: case Op::sw: case Op::sd: case Op::sq:
        case Op::swl: case Op::swr: case Op::sdl: case Op::sdr:
            return {true, std::nullopt, false, bit(rs) | bit(rt)};
        case Op::lwc1: case Op::lqc2: case Op::swc1: case Op::sqc2:
            return {true, std::nullopt, false, bit(rs)};

        case Op::beq: case Op::bne: case Op::beql: case Op::bnel:
            return {true, std::nullopt, false, bit(rs) | bit(rt)};
        case Op::blez: case Op::bgtz: case Op::bltz: case Op::bgez:
        case Op::blezl: case Op::bgtzl: case Op::bltzl: case Op::bgezl:
        case Op::jr:
            return {true, std::nullopt, false, bit(rs)};
        case Op::j: return {true, std::nullopt, false, 0};
        case Op::jal: return {true, reg::ra, false, 0};
        case Op::jalr: return {true, rd, false, bit(rs)};

        case Op::mfc0: case Op::mfc1: case Op::cfc1: case Op::cfc2_ni:
            return {true, rt, false, 0};
        case Op::qmfc2_ni: return {true, rt, true, 0};
        case Op::mtc0: case Op::mtc1: case Op::ctc1:
        case Op::qmtc2_ni: case Op::ctc2_ni:
            return {true, std::nullopt, false, bit(rt)};
        case Op::mthi: case Op::mtlo: case Op::mthi1: case Op::mtlo1:
        case Op::mtsa: case Op::mtsab: case Op::mtsah:
        case Op::pmthi: case Op::pmtlo: case Op::pmthl_lw:
            return {true, std::nullopt, false, bit(rs)};
        case Op::mfhi: case Op::mflo: case Op::mfhi1: case Op::mflo1: case Op::mfsa:
            return {true, rd, false, 0};
        case Op::pmfhi: case Op::pmflo:
        case Op::pmfhl_lw: case Op::pmfhl_uw: case Op::pmfhl_slw:
        case Op::pmfhl_lh: case Op::pmfhl_sh:
            return {true, rd, true, 0};

        case Op::div: case Op::divu: case Op::div1: case Op::divu1:
        case Op::pdivw: case Op::pdivuw: case Op::pdivbw:
        case Op::teq: case Op::tge: case Op::tgeu:
        case Op::tlt: case Op::tltu: case Op::tne:
            return {true, std::nullopt, false, bit(rs) | bit(rt)};
        case Op::mult: case Op::multu: case Op::mult1: case Op::multu1:
        case Op::madd: case Op::madd1: case Op::maddu: case Op::maddu1:
            return {true, rd, false, bit(rs) | bit(rt)};

        case Op::di: case Op::ei:
        case Op::cache: case Op::pref: case Op::sync_p:
        case Op::tlbwi: case Op::tlbwr: case Op::tlbr: case Op::tlbp:
            return {true, std::nullopt, false, 0};

        default:
            if (writesCoprocessorOnly(op)) {
                return {true, std::nullopt, false, 0};
            }
            return {};
    }
}

}  // namespace

// lui is already a constant in the emitted form, so folding it saves nothing.
std::optional<KnownWrite> foldConstant(const BlockValues& values, Op op, Instruction insn) {
    if (op == Op::lui) {
        return std::nullopt;
    }
    const Folded folded = fold(values, op, insn);
    if (!folded.modelled || !folded.value || folded.destination == 0) {
        return std::nullopt;
    }
    return KnownWrite{folded.destination, *folded.value};
}

void BlockValues::apply(Op op, Instruction insn, u32 callClobber) {
    mCallClobber = callClobber;
    applyWrites(op, insn);

    // Only the R3000A delays a load, but tracking it on both costs just the
    // pattern that writes a register right after loading it.
    const u32 arriving = std::exchange(mLoadedPreviously, kNoRegister);
    if (arriving != kNoRegister) {
        forget(arriving);
    }
    if (isLoad(op)) {
        mLoadedPreviously = insn.rt();
    }
}

void BlockValues::applyWrites(Op op, Instruction insn) {
    if (op == Op::lw && mImage != nullptr) {
        if (const std::optional<u32> address = getAddress(insn)) {
            if (const std::optional<u32> word = mImage->read32(*address)) {
                setRegister(insn.rt(), *word);
                return;
            }
        }
    }
    const Folded folded = fold(*this, op, insn);
    if (folded.modelled) {
        if (folded.value) {
            setRegister(folded.destination, *folded.value);
        } else if (const std::optional<u32> base = carriedBase(*this, op, insn)) {
            // The base stays known, which says where the access lands.
            setBase(folded.destination, *base);
        } else {
            forget(folded.destination);
        }
        return;
    }
    if (writesNoRegister(op) || writesCoprocessorOnly(op)) {
        return;
    }
    if (linksToReturnAddress(op)) {
        for (u32 reg = 1; reg < 32; ++reg) {
            if ((mCallClobber >> reg) & 1u) {
                forget(reg);
            }
        }
        if (op == Op::jalr) {
            forget(insn.rd());
        }
        return;
    }
    if (writesTargetOnly(op)) {
        forget(insn.rt());
        return;
    }
    if (writesDestinationOnly(op)) {
        forget(insn.rd());
        return;
    }
    if (op == Op::syscall) {
        // Set once, so later syscalls in the block still know it.
        const std::optional<u32> number = getRegister(kSyscallNumberRegister);
        forgetAll();
        if (number) {
            setRegister(kSyscallNumberRegister, *number);
        }
        return;
    }
    forgetAll();
}

namespace {

// A block runs to its first control transfer, delay slot included.
struct Walk {
    struct Step {
        u32 pc = 0;
        Instruction insn;
        Op op = Op::Invalid;
        BlockValues before;
    };

    std::vector<Step> steps;
    ControlFlow flow;
    u32 flowPc = 0;
    bool hasDelaySlot = false;
    BlockValues after;
    BlockValues afterWithoutSlot;
    u32 fallthrough = 0;
};

std::optional<Instruction> fetchAt(Memory& memory, u32 pc) {
    const std::optional<u32> word = memory.peek32(pc);
    return word ? std::optional(Instruction{*word}) : std::nullopt;
}

Walk walkBlock(Memory& memory, const CodeMap& code, u32 blockStart, const BlockValues& entry,
               const std::map<u32, u32>* clobbered = nullptr) {
    const auto getClobber = [&](u32 pc, Instruction insn, Op op) {
        const ControlFlow cf = getControlFlow(op, insn, pc);
        if (cf.link == 0 || cf.flow != Flow::Jump || clobbered == nullptr) {
            return kCallerSaved;
        }
        const auto found = clobbered->find(cf.target);
        return found == clobbered->end() ? kCallerSaved : found->second;
    };
    Walk walk;
    BlockValues values = entry;
    u32 pc = blockStart;
    for (;;) {
        const std::optional<Instruction> insn = fetchAt(memory, pc);
        if (!insn) {
            walk.after = values;
            walk.afterWithoutSlot = values;
            return walk;
        }
        const Op op = getOp(*insn);
        walk.steps.push_back({pc, *insn, op, values});

        const ControlFlow cf = getControlFlow(op, *insn, pc);
        if (cf.flow != Flow::Next && !(cf.flow == Flow::Trap && op == Op::syscall)) {
            walk.flow = cf;
            walk.flowPc = pc;
            // The transfer's own register write happens before the slot runs.
            values.apply(op, *insn, getClobber(pc, *insn, op));
            walk.afterWithoutSlot = values;
            if (cf.flow != Flow::Trap) {
                walk.hasDelaySlot = true;
                const std::optional<Instruction> slot = fetchAt(memory, pc + 4);
                if (slot) {
                    walk.steps.push_back({pc + 4, *slot, getOp(*slot), values});
                    values.apply(getOp(*slot), *slot, getClobber(pc + 4, *slot, getOp(*slot)));
                }
            }
            walk.after = values;
            return walk;
        }
        values.apply(op, *insn, getClobber(pc, *insn, op));
        pc += 4;
        if (code.isBlockStart(pc)) {
            walk.fallthrough = pc;
            walk.after = values;
            walk.afterWithoutSlot = values;
            return walk;
        }
    }
}

}  // namespace

std::map<u32, BlockValues> analyseBlock(Memory& memory, const CodeMap& code, u32 blockStart,
                                        const BlockValues& entry) {
    std::map<u32, BlockValues> out;
    for (const Walk::Step& step : walkBlock(memory, code, blockStart, entry).steps) {
        out[step.pc] = step.before;
    }
    return out;
}

// What a block leaves behind is live unless the whole function is in view.
namespace {

// Followed by half: a read revives both, a narrow write settles only the low.
struct Live {
    u32 low = 0;
    u32 high = 0;

    static constexpr Live all() { return {0xFFFFFFFFu, 0xFFFFFFFFu}; }
    bool merge(const Live& other) {
        const Live before = *this;
        low |= other.low;
        high |= other.high;
        return low != before.low || high != before.high;
    }
};

struct BlockLiveness {
    std::set<u32> dead;
    Live in;
};

BlockLiveness scanBlock(const Walk& walk, LoadDelay delay, Live out) {
    struct Use {
        u32 pc;
        RegisterUse use;
    };

    // A likely branch runs its slot only when taken, so it kills no earlier write.
    const bool slotMayNotRun = walk.flow.flow == Flow::BranchLikely;

    std::vector<Use> uses;
    for (const Walk::Step& step : walk.steps) {
        // A syscall hands every register to the kernel.
        if (step.op == Op::syscall) {
            uses.clear();
            continue;
        }
        // A folded instruction reads nothing, so its operands are dead.
        const std::optional<KnownWrite> folded = foldConstant(step.before, step.op, step.insn);
        RegisterUse use =
            folded ? RegisterUse{true, folded->reg, 0} : useOf(step.op, step.insn);
        if (takesResolvedBase(step.op) && step.before.getRegister(step.insn.rs())) {
            use.reads &= ~bit(step.insn.rs());
        }
        if (slotMayNotRun && step.pc == walk.flowPc + 4) {
            use.writes = std::nullopt;
        }
        // The register still holds the old value when the block hands it on.
        if (delay == LoadDelay::OneInstruction && isLoad(step.op)) {
            use.writes = std::nullopt;
        }
        uses.push_back({step.pc, use});
    }

    BlockLiveness result;
    Live live = out;
    for (auto use = uses.rbegin(); use != uses.rend(); ++use) {
        if (!use->use.complete) {
            live = Live::all();
            continue;
        }
        if (use->use.writes) {
            const u32 written = bit(*use->use.writes);
            const u32 wanted = use->use.writesWholeRegister ? (live.low | live.high) : live.low;
            if ((wanted & written) == 0 && use->use.reads == 0) {
                result.dead.insert(use->pc);
                continue;
            }
            live.low &= ~written;
            if (use->use.writesWholeRegister) {
                live.high &= ~written;
            }
        }
        live.low |= use->use.reads;
        live.high |= use->use.reads;
    }
    result.in = live;
    return result;
}

}  // namespace

std::map<u32, std::set<u32>> findDeadWritesInFunction(
    Memory& memory, const CodeMap& code, const std::vector<u32>& blocks,
    const std::map<u32, BlockValues>& entries, LoadDelay delay) {
    const BlockValues nothingKnown;
    const auto entryOf = [&](u32 block) -> const BlockValues& {
        const auto found = entries.find(block);
        return found == entries.end() ? nothingKnown : found->second;
    };

    std::map<u32, Walk> walks;
    std::map<u32, Live> liveIn;
    for (u32 block : blocks) {
        walks.emplace(block, walkBlock(memory, code, block, entryOf(block)));
        liveIn.emplace(block, Live{});
    }

    // Handed to code this pass cannot see, so anything may be read next.
    const auto liveAtExitOf = [&](u32 block) {
        const std::vector<u32>& successors = code.getSuccessors(block);
        if (walks.at(block).flow.link != 0 || successors.empty()) {
            return Live::all();
        }
        Live out;
        for (u32 successor : successors) {
            const auto found = liveIn.find(successor);
            if (found == liveIn.end()) {
                return Live::all();
            }
            out.merge(found->second);
        }
        return out;
    };

    // A block reached several ways ends up asked for everything any of them wants.
    for (bool changed = true; changed;) {
        changed = false;
        for (auto block = blocks.rbegin(); block != blocks.rend(); ++block) {
            changed |= liveIn.at(*block).merge(
                scanBlock(walks.at(*block), delay, liveAtExitOf(*block)).in);
        }
    }

    std::map<u32, std::set<u32>> dead;
    for (u32 block : blocks) {
        dead[block] = scanBlock(walks.at(block), delay, liveAtExitOf(block)).dead;
    }
    return dead;
}

// A call hands every register away; a likely branch runs its slot on one edge.
bool keepsReturnAddress(Memory& memory, const CodeMap& code, u32 function,
                        const std::vector<u32>& blocks) {
    if (code.hasOpenJump(function)) {
        return false;
    }
    for (u32 block : blocks) {
        const Walk walk = walkBlock(memory, code, block, BlockValues{});
        if (walk.flow.link != 0) {
            return false;
        }
        for (const Walk::Step& step : walk.steps) {
            const RegisterUse use = useOf(step.op, step.insn);
            if (!use.complete || use.writes == reg::ra) {
                return false;
            }
        }
    }
    return true;
}

// An indirect call, or an instruction this cannot describe, means the whole
// caller-saved set.
std::map<u32, u32> findClobberedRegisters(Memory& memory, const CodeMap& code) {
    std::map<u32, u32> clobbered;
    std::map<u32, std::set<u32>> calls;
    for (const auto& [function, blockStarts] : code.getBlocksByFunction()) {
        u32 mask = 0;
        for (const u32 block : blockStarts) {
            for (u32 pc = block;; pc += 4) {
                const std::optional<Instruction> insn = fetchAt(memory, pc);
                if (!insn) {
                    mask = kCallerSaved;
                    break;
                }
                const Op op = getOp(*insn);
                // A tail call is such an edge, and so is a recovered jump table.
                const ControlFlow cf = getControlFlow(op, *insn, pc);
                if (cf.flow == Flow::Jump) {
                    if (code.hasBody(cf.target)) {
                        calls[function].insert(cf.target);
                    } else {
                        mask |= kCallerSaved;
                    }
                } else if (cf.flow == Flow::JumpRegister && cf.source != reg::ra) {
                    const std::vector<u32>* targets = code.getJumpTable(pc);
                    if (targets != nullptr && !targets->empty()) {
                        for (const u32 target : *targets) {
                            code.hasBody(target) ? (void)calls[function].insert(target)
                                                 : (void)(mask |= kCallerSaved);
                        }
                    } else {
                        mask |= kCallerSaved;
                    }
                }
                const RegisterUse use = useOf(op, *insn);
                if (!use.complete) {
                    mask = kCallerSaved;
                } else if (use.writes) {
                    mask |= bit(*use.writes);
                }
                if (cf.flow != Flow::Next || code.isBlockStart(pc + 4)) {
                    break;
                }
            }
        }
        clobbered[function] = mask & kCallerSaved;
    }

    for (bool changed = true; changed;) {
        changed = false;
        for (const auto& [function, targets] : calls) {
            u32 mask = clobbered[function];
            for (const u32 target : targets) {
                mask |= clobbered.count(target) != 0 ? clobbered[target] : kCallerSaved;
            }
            mask &= kCallerSaved;
            if (mask != clobbered[function]) {
                clobbered[function] = mask;
                changed = true;
            }
        }
    }
    return clobbered;
}

std::map<u32, BlockValues> analyseFunction(Memory& memory, const CodeMap& code, u32 function,
                                           const std::vector<u32>& blocks,
                                           const BlockValues& always,
                                           const std::map<u32, u32>* clobbered) {
    std::map<u32, BlockValues> entryOf;
    for (u32 block : blocks) {
        entryOf[block] = always;
    }
    if (code.hasOpenJump(function)) {
        return entryOf;
    }

    std::set<u32> reached{function};
    std::vector<u32> pending{function};
    while (!pending.empty()) {
        const u32 block = pending.back();
        pending.pop_back();
        const Walk walk = walkBlock(memory, code, block, entryOf[block], clobbered);

        std::vector<std::pair<u32, const BlockValues*>> edges;
        const BlockValues nothing;
        if (walk.fallthrough != 0) {
            edges.push_back({walk.fallthrough, &walk.after});
        } else if (walk.flow.link != 0) {
            edges.push_back({walk.flowPc + 8, &nothing});
        } else if (walk.flow.flow == Flow::Branch) {
            edges.push_back({walk.flow.target, &walk.after});
            edges.push_back({walk.flowPc + 8, &walk.after});
        } else if (walk.flow.flow == Flow::BranchLikely) {
            edges.push_back({walk.flow.target, &walk.after});
            edges.push_back({walk.flowPc + 8, &walk.afterWithoutSlot});
        } else if (walk.flow.flow == Flow::Jump) {
            edges.push_back({walk.flow.target, &walk.after});
        } else if (walk.flow.flow == Flow::JumpRegister) {
            if (const std::vector<u32>* table = code.getJumpTable(walk.flowPc)) {
                for (u32 target : *table) {
                    edges.push_back({target, &walk.after});
                }
            }
        }

        for (const auto& [successor, values] : edges) {
            if (!code.owns(function, successor) || entryOf.count(successor) == 0) {
                continue;
            }
            if (reached.insert(successor).second) {
                entryOf[successor] = *values;
                pending.push_back(successor);
            } else if (entryOf[successor].meet(*values)) {
                pending.push_back(successor);
            }
        }
    }

    // Entered some way this cannot see, so only what holds program-wide does.
    for (u32 block : blocks) {
        if (reached.count(block) == 0) {
            entryOf[block] = always;
        }
    }
    return entryOf;
}

}  // namespace uee
