#include "vu_translate.h"

#include "vu_handlers.h"
#include "vu_micro.h"
#include "vu_micro_ops.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace uee {
namespace {

struct Pair {
    u32 upper = 0;
    u32 lower = 0;
    bool ends = false;
    VuTransfer transfer = VuTransfer::None;
    u32 target = 0;
};

Pair readPair(std::span<const u8> code, u32 count, u32 pc) {
    Pair pair;
    std::memcpy(&pair.lower, code.data() + pc * 8, 4);
    std::memcpy(&pair.upper, code.data() + pc * 8 + 4, 4);
    pair.ends = (pair.upper & kVuEndBit) != 0;
    const VuLower lower = decodeLower(pair.upper, pair.lower);
    pair.transfer = getTransfer(lower);
    pair.target = (pc + 1 + static_cast<u32>(lower.offset)) % count;
    return pair;
}

// A set of a record's pairs and where they branch: everything its start can
// reach, or only what ran. A jump target the record never saw is left to the
// interpreter.
class Discovery {
public:
    Discovery(const VuProgramRecord& record, const std::vector<bool>& pairs)
        : mCode(record.code), mCount(static_cast<u32>(record.code.size() / 8)),
          mJumps(record.jumps) {
        for (u32 pc = 0; pc < mCount; ++pc) {
            if (pairs[pc]) {
                mIncluded.insert(pc);
            }
        }
        for (const u32 pc : mIncluded) {
            const u32 next = (pc + 1) % mCount;
            if (getPair(pc).transfer != VuTransfer::None && mIncluded.contains(next) &&
                getPair(next).transfer != VuTransfer::None) {
                mBranchInSlot = true;
            }
        }
    }

    std::vector<u32> getTargets(u32 pc, const Pair& pair) const {
        switch (pair.transfer) {
            case VuTransfer::Jump: return {pair.target};
            case VuTransfer::Branch: return {pair.target, (pc + 2) % mCount};
            case VuTransfer::Indirect: {
                const auto found = mJumps.find(pc);
                return found == mJumps.end() ? std::vector<u32>{}
                                             : std::vector<u32>(found->second.begin(),
                                                                found->second.end());
            }
            case VuTransfer::None: break;
        }
        return {};
    }

    Pair getPair(u32 pc) const { return readPair(mCode, mCount, pc); }
    const std::set<u32>& getIncluded() const { return mIncluded; }
    bool hasBranchInSlot() const { return mBranchInSlot; }
    u32 getCount() const { return mCount; }

private:
    std::span<const u8> mCode;
    u32 mCount;
    const std::map<u32, std::set<u32>>& mJumps;
    std::set<u32> mIncluded;
    bool mBranchInSlot = false;
};

struct ProgramSource {
    std::string name;
    bool fast = false;
    // The full translation the name hands over to, when what ran is not all.
    std::string fallback;
    u32 unit;
    u32 start;
    u64 hash;
    std::vector<VuPairRange> ranges;
    std::string body;
};

constexpr usize kCommentColumn = 60;

std::string label(u32 pc) {
    return std::format("L_{:04x}", pc);
}

// A statement with the instruction it came from beside it, the way the EE and
// IOP translations read.
std::string annotate(std::string_view indent, const std::string& statement,
                     const std::string& comment) {
    std::string line = std::string(indent) + statement;
    if (line.size() < kCommentColumn) {
        line.append(kCommentColumn - line.size(), ' ');
    } else {
        line += ' ';
    }
    return line + "// " + comment + "\n";
}

// Where control can go after the pair at pc. Only a pair behind a branch can
// go anywhere but the next one, unless a branch sits in a slot somewhere.
std::vector<u32> getSuccessors(const Discovery& discovery, u32 pc) {
    const u32 count = discovery.getCount();
    const u32 next = (pc + 1) % count;
    const std::set<u32>& included = discovery.getIncluded();
    if (discovery.hasBranchInSlot()) {
        return {included.begin(), included.end()};
    }
    std::vector<u32> successors;
    if (included.contains(next)) {
        successors.push_back(next);
    }
    const u32 previous = (pc + count - 1) % count;
    const Pair before = discovery.getPair(previous);
    if (before.transfer != VuTransfer::None && included.contains(previous)) {
        for (const u32 target : discovery.getTargets(previous, before)) {
            if (included.contains(target) &&
                std::find(successors.begin(), successors.end(), target) == successors.end()) {
                successors.push_back(target);
            }
        }
    }
    return successors;
}

bool isDispatched(const Discovery& discovery, u32 pc) {
    if (discovery.hasBranchInSlot()) {
        return true;
    }
    const u32 count = discovery.getCount();
    const u32 previous = (pc + count - 1) % count;
    return discovery.getIncluded().contains(previous) &&
           discovery.getPair(previous).transfer != VuTransfer::None;
}

bool overlaps(const VuLanes& a, const VuLanes& b) {
    return a.vf != 0 && a.vf == b.vf && (a.lanes & b.lanes) != 0;
}

bool writesLanes(const VuPairTiming& timing, const VuLanes& read) {
    return overlaps(timing.upperWrite, read) || overlaps(timing.lowerWrite, read);
}

bool readsLanes(const VuPairTiming& timing, const VuLanes& write) {
    for (const VuLanes& read : {timing.upperReads[0], timing.upperReads[1], timing.lowerReads[0],
                                timing.lowerReads[1]}) {
        if (overlaps(read, write)) {
            return true;
        }
    }
    return false;
}

// Which pairs can run next to which. A result is ready kFmacLatency cycles on
// and every pair takes one at least, so only that many pairs either way can
// see a stall, and a check or mark with none of them in reach is dropped.
class PairFlow {
public:
    PairFlow(const Discovery& discovery, u32 start) {
        const std::set<u32>& included = discovery.getIncluded();
        const u32 count = discovery.getCount();
        for (const u32 pc : included) {
            mTimings[pc] = getPairTiming(discovery.getPair(pc).upper, discovery.getPair(pc).lower);
            const u32 next = (pc + 1) % count;
            if (isDispatched(discovery, pc)) {
                mSuccessors[pc] = getSuccessors(discovery, pc);
            } else if (included.contains(next)) {
                mSuccessors[pc] = {next};
            } else {
                mExits.insert(pc);
            }
            for (const u32 successor : mSuccessors[pc]) {
                mPredecessors[successor].push_back(pc);
            }
        }
        for (const u32 pc : included) {
            if (isDispatched(discovery, pc) && canMissDispatch(discovery, pc, start)) {
                mExits.insert(pc);
            }
        }
    }

    const VuPairTiming& getTiming(u32 pc) const { return mTimings.at(pc); }
    bool isExit(u32 pc) const { return mExits.contains(pc); }

    const std::vector<u32>& getPredecessors(u32 pc) const {
        static const std::vector<u32> kNone;
        const auto found = mPredecessors.find(pc);
        return found == mPredecessors.end() ? kNone : found->second;
    }

    // Whether a pair fewer than latency pairs back matches.
    template <typename Match>
    bool isBefore(u32 pc, u32 latency, Match match) const {
        std::set<u32> frontier{pc};
        for (u32 depth = 1; depth < latency; ++depth) {
            frontier = step(frontier, mPredecessors);
            for (const u32 before : frontier) {
                if (match(getTiming(before))) {
                    return true;
                }
            }
        }
        return false;
    }

    // The same looking ahead. The interpreter reads the marks too once it
    // takes over, so reaching an exit counts as a match.
    template <typename Match>
    bool isAfter(u32 pc, u32 latency, Match match) const {
        std::set<u32> frontier{pc};
        for (u32 depth = 1; depth < latency; ++depth) {
            for (const u32 exit : frontier) {
                if (mExits.contains(exit)) {
                    return true;
                }
            }
            frontier = step(frontier, mSuccessors);
            for (const u32 after : frontier) {
                if (match(getTiming(after))) {
                    return true;
                }
            }
        }
        return false;
    }

private:
    // The dispatch after a delay slot falls to the interpreter when the counter
    // it gets is none of the cases: an unknown jump, or a pair that was not found.
    bool canMissDispatch(const Discovery& discovery, u32 pc, u32 start) const {
        const u32 count = discovery.getCount();
        const u32 previous = (pc + count - 1) % count;
        const Pair branch = discovery.getPair(previous);
        if (discovery.hasBranchInSlot() || branch.transfer == VuTransfer::Indirect) {
            return true;
        }
        const std::set<u32>& included = discovery.getIncluded();
        for (const u32 target : discovery.getTargets(previous, branch)) {
            if (!included.contains(target)) {
                return true;
            }
        }
        const auto found = mPredecessors.find(pc);
        const bool enteredElsewhere =
            pc == start || (found != mPredecessors.end() &&
                            std::any_of(found->second.begin(), found->second.end(),
                                        [&](u32 before) { return before != previous; }));
        const bool fallsThrough = branch.transfer == VuTransfer::Branch || enteredElsewhere;
        return fallsThrough && !included.contains((pc + 1) % count);
    }

    static std::set<u32> step(const std::set<u32>& frontier,
                              const std::map<u32, std::vector<u32>>& edges) {
        std::set<u32> next;
        for (const u32 pc : frontier) {
            const auto found = edges.find(pc);
            if (found != edges.end()) {
                next.insert(found->second.begin(), found->second.end());
            }
        }
        return next;
    }

    std::map<u32, VuPairTiming> mTimings;
    std::map<u32, std::vector<u32>> mSuccessors;
    std::map<u32, std::vector<u32>> mPredecessors;
    std::set<u32> mExits;
};

bool readsInteger(const VuPairTiming& timing, u32 vi) {
    for (u32 i = 0; i < timing.integerReadCount; ++i) {
        if (timing.integerReads[i] == vi) {
            return true;
        }
    }
    return false;
}

// The checks and marks a pair still needs once the flow has ruled some out.
VuPairTiming pruneTiming(const PairFlow& flow, u32 pc) {
    VuPairTiming timing = flow.getTiming(pc);
    constexpr u32 kLanes = VuMicro::kFmacLatency;
    for (VuLanes* read : {&timing.upperReads[0], &timing.upperReads[1], &timing.lowerReads[0],
                          &timing.lowerReads[1]}) {
        const auto writes = [&](const VuPairTiming& t) { return writesLanes(t, *read); };
        if (!flow.isBefore(pc, kLanes, writes)) {
            *read = {};
        }
    }
    for (VuLanes* write : {&timing.upperWrite, &timing.lowerWrite}) {
        const auto reads = [&](const VuPairTiming& t) { return readsLanes(t, *write); };
        if (!flow.isAfter(pc, kLanes, reads)) {
            *write = {};
        }
    }

    constexpr u32 kLoad = VuMicro::kIntegerLoadLatency;
    u32 kept = 0;
    for (u32 i = 0; i < timing.integerReadCount; ++i) {
        const u32 vi = timing.integerReads[i];
        if (flow.isBefore(pc, kLoad, [&](const VuPairTiming& t) {
                return t.loadsInteger && t.integerLoad == vi;
            })) {
            timing.integerReads[kept++] = vi;
        }
    }
    timing.integerReadCount = kept;
    if (timing.loadsInteger &&
        !flow.isAfter(pc, kLoad, [&](const VuPairTiming& t) {
            return readsInteger(t, timing.integerLoad);
        })) {
        timing.loadsInteger = false;
    }
    return timing;
}

// The flag records some read can see. A reader sees the newest record at least
// kFlagDelay cycles old, and a record that many pairs back is always that old,
// so a walk back from each reader stops at the first record that far away. A
// pair that hands over to the interpreter counts as a reader just after it.
std::set<u32> getLiveRecords(const Discovery& discovery, const PairFlow& flow) {
    std::set<u32> recording;
    std::vector<std::pair<u32, u32>> work;
    for (const u32 pc : discovery.getIncluded()) {
        const Pair pair = discovery.getPair(pc);
        const VuLower lower = decodeLower(pair.upper, pair.lower);
        if (changesFlags(decodeUpper(pair.upper), lower)) {
            recording.insert(pc);
        }
        if (readsFlags(lower)) {
            for (const u32 before : flow.getPredecessors(pc)) {
                work.push_back({before, 1});
            }
        }
        if (flow.isExit(pc)) {
            work.push_back({pc, 1});
        }
    }
    std::set<u32> live;
    std::set<std::pair<u32, u32>> visited;
    while (!work.empty()) {
        const auto [pc, distance] = work.back();
        work.pop_back();
        if (!visited.insert({pc, std::min(distance, VuMicro::kFlagDelay)}).second) {
            continue;
        }
        if (recording.contains(pc)) {
            live.insert(pc);
            if (distance >= VuMicro::kFlagDelay) {
                continue;
            }
        }
        for (const u32 before : flow.getPredecessors(pc)) {
            work.push_back({before, distance + 1});
        }
    }
    return live;
}

// Where something jumps: the start, whatever a dispatch after a delay slot can
// pick, and a next pair that does not follow in the order written.
std::set<u32> getLabels(const Discovery& discovery, const std::vector<u32>& order, u32 start) {
    const u32 count = discovery.getCount();
    std::set<u32> labels{start};
    for (usize i = 0; i < order.size(); ++i) {
        const u32 pc = order[i];
        const u32 next = (pc + 1) % count;
        if (isDispatched(discovery, pc)) {
            for (const u32 successor : getSuccessors(discovery, pc)) {
                labels.insert(successor);
            }
        } else if (discovery.getIncluded().contains(next) &&
                   (i + 1 == order.size() || order[i + 1] != next)) {
            labels.insert(next);
        }
    }
    return labels;
}

// Pairs that may run right after an end bit, where the program can stop.
std::set<u32> getPairsAfterEnd(const Discovery& discovery) {
    const u32 count = discovery.getCount();
    std::set<u32> after;
    for (const u32 pc : discovery.getIncluded()) {
        if (!discovery.getPair(pc).ends) {
            continue;
        }
        after.insert((pc + 1) % count);
        if (isDispatched(discovery, pc)) {
            for (const u32 successor : getSuccessors(discovery, pc)) {
                after.insert(successor);
            }
        }
    }
    return after;
}

std::string_view getSourceName(VuSource source) {
    switch (source) {
        case VuSource::X: return "X";
        case VuSource::Y: return "Y";
        case VuSource::Z: return "Z";
        case VuSource::W: return "W";
        case VuSource::Q: return "Q";
        case VuSource::I: return "I";
        default: return "Vt";
    }
}

// Each form a program uses gets its own copy of the op, with the source, the
// lanes and the target fixed.
std::string_view getVectorOp(bool fast) {
    return fast ? "vectorOpFast" : "vectorOpFor";
}

std::string emitVector(std::string_view kind, const VuUpper& u, bool fast) {
    return std::format("{}<VuKind::{}, VuSource::{}, false, 0x{:x}>(s, {}, {}, {});",
                       getVectorOp(fast), kind, getSourceName(u.source), u.dest, u.fd, u.fs,
                       u.ft);
}

std::string emitAccumulate(std::string_view kind, const VuUpper& u, bool fast) {
    return std::format("{}<VuKind::{}, VuSource::{}, true, 0x{:x}>(s, 0, {}, {});",
                       getVectorOp(fast), kind, getSourceName(u.source), u.dest, u.fs, u.ft);
}

// The upper ops are the unit's register file work, so they take the state; the
// lower ones reach the pipelines too, so they take the machine.
// The fast mode swaps the float arithmetic for the host's, see vectorOpFast.
std::string emitUpper(const VuUpper& u, bool fast) {
    switch (u.op) {
        case VuUpperOp::None: return {};
        case VuUpperOp::Add: return emitVector("Add", u, fast);
        case VuUpperOp::Sub: return emitVector("Sub", u, fast);
        case VuUpperOp::Madd: return emitVector("Madd", u, fast);
        case VuUpperOp::Msub: return emitVector("Msub", u, fast);
        case VuUpperOp::Max: return emitVector("Max", u, fast);
        case VuUpperOp::Mini: return emitVector("Mini", u, fast);
        case VuUpperOp::Mul: return emitVector("Mul", u, fast);
        case VuUpperOp::Adda: return emitAccumulate("Add", u, fast);
        case VuUpperOp::Suba: return emitAccumulate("Sub", u, fast);
        case VuUpperOp::Madda: return emitAccumulate("Madd", u, fast);
        case VuUpperOp::Msuba: return emitAccumulate("Msub", u, fast);
        case VuUpperOp::Mula: return emitAccumulate("Mul", u, fast);
        case VuUpperOp::Opmsub:
            return std::format("vopmsub{}(s, 0x{:x}, {}, {}, {});", fast ? "Fast" : "", u.dest,
                               u.fd, u.fs, u.ft);
        case VuUpperOp::Opmula:
            return std::format("vopmula{}(s, 0x{:x}, {}, {});", fast ? "Fast" : "", u.dest, u.fs,
                               u.ft);
        case VuUpperOp::Itof:
            return std::format("vitof(s, 0x{:x}, {}, {}, {});", u.dest, u.ft, u.fs, u.shift);
        case VuUpperOp::Ftoi:
            return std::format("vftoi(s, 0x{:x}, {}, {}, {});", u.dest, u.ft, u.fs, u.shift);
        case VuUpperOp::Abs: return std::format("vabs(s, 0x{:x}, {}, {});", u.dest, u.ft, u.fs);
        case VuUpperOp::Clip: return std::format("vclip(s, {}, {});", u.fs, u.ft);
        case VuUpperOp::Move: return std::format("vmove(s, 0x{:x}, {}, {});", u.dest, u.ft, u.fs);
    }
    return {};
}

// Memory ops name the unit, which folds the checks that depend on it.
std::string emitLower(u32 pc, const VuLower& l, bool fast, u32 unit) {
    const std::string_view suffix = fast ? "Fast" : "";
    switch (l.op) {
        case VuLowerOp::None: return {};
        case VuLowerOp::Immediate: return std::format("setImmediate(m, 0x{:x});", l.immediate);
        case VuLowerOp::Move: return std::format("vmove(s, 0x{:x}, {}, {});", l.dest, l.ft, l.fs);
        case VuLowerOp::Mr32: return std::format("vmr32(s, 0x{:x}, {}, {});", l.dest, l.ft, l.fs);
        case VuLowerOp::Mfir: return std::format("vmfir(s, 0x{:x}, {}, {});", l.dest, l.ft, l.fs);
        case VuLowerOp::Mtir: return std::format("vmtir(s, {}, {}, {});", l.ft, l.fs, l.fsLane);
        case VuLowerOp::Rinit: return std::format("rinit(m, {}, {});", l.fs, l.fsLane);
        case VuLowerOp::Rxor: return std::format("rxor(m, {}, {});", l.fs, l.fsLane);
        case VuLowerOp::Rnext: return std::format("rnext(m, 0x{:x}, {});", l.dest, l.ft);
        case VuLowerOp::Rget: return std::format("vrget(s, 0x{:x}, {});", l.dest, l.ft);
        case VuLowerOp::Mfp: return std::format("mfp(m, 0x{:x}, {});", l.dest, l.ft);
        case VuLowerOp::Xgkick: return std::format("xgkick(m, {});", l.fs);
        case VuLowerOp::Xtop: return std::format("xtop(m, {});", l.ft);
        case VuLowerOp::Xitop: return std::format("xitop(m, {});", l.ft);
        case VuLowerOp::Function:
            return std::format("efu(m, 0x{:x}, {}, {});", l.function, l.fs, l.fsLane);
        case VuLowerOp::Div:
            return std::format("div{}(m, {}, {}, {}, {});", suffix, l.fs, l.fsLane, l.ft,
                               l.ftLane);
        case VuLowerOp::Sqrt: return std::format("sqrt{}(m, {}, {});", suffix, l.ft, l.ftLane);
        case VuLowerOp::Rsqrt:
            return std::format("rsqrt{}(m, {}, {}, {}, {});", suffix, l.fs, l.fsLane, l.ft,
                               l.ftLane);
        case VuLowerOp::Ilwr:
            return std::format("ilwr<{}>(m, 0x{:x}, {}, {});", unit, l.dest, l.ft, l.fs);
        case VuLowerOp::Iswr:
            return std::format("iswr<{}>(m, 0x{:x}, {}, {});", unit, l.dest, l.ft, l.fs);
        case VuLowerOp::Lqi:
            return std::format("lqi<{}>(m, 0x{:x}, {}, {});", unit, l.dest, l.ft, l.fs);
        case VuLowerOp::Sqi:
            return std::format("sqi<{}>(m, 0x{:x}, {}, {});", unit, l.dest, l.fs, l.ft);
        case VuLowerOp::Lqd:
            return std::format("lqd<{}>(m, 0x{:x}, {}, {});", unit, l.dest, l.ft, l.fs);
        case VuLowerOp::Sqd:
            return std::format("sqd<{}>(m, 0x{:x}, {}, {});", unit, l.dest, l.fs, l.ft);
        case VuLowerOp::Iadd: return std::format("viadd(s, {}, {}, {});", l.fd, l.fs, l.ft);
        case VuLowerOp::Isub: return std::format("visub(s, {}, {}, {});", l.fd, l.fs, l.ft);
        case VuLowerOp::Iand: return std::format("viand(s, {}, {}, {});", l.fd, l.fs, l.ft);
        case VuLowerOp::Ior: return std::format("vior(s, {}, {}, {});", l.fd, l.fs, l.ft);
        case VuLowerOp::Iaddi: return std::format("viaddi(s, {}, {}, {});", l.ft, l.fs, l.offset);
        case VuLowerOp::Lq:
            return std::format("lq<{}>(m, 0x{:x}, {}, {}, {});",
                                   unit, l.dest, l.ft, l.fs, l.offset);
        case VuLowerOp::Sq:
            return std::format("sq<{}>(m, 0x{:x}, {}, {}, {});",
                                   unit, l.dest, l.fs, l.ft, l.offset);
        case VuLowerOp::Fcget: return std::format("fcget(m, {});", l.ft);
        case VuLowerOp::Fcset: return std::format("fcset(m, 0x{:x});", l.immediate);
        case VuLowerOp::Fcand: return std::format("fcand(m, 0x{:x});", l.immediate);
        case VuLowerOp::Fcor: return std::format("fcor(m, 0x{:x});", l.immediate);
        case VuLowerOp::Fceq: return std::format("fceq(m, 0x{:x});", l.immediate);
        case VuLowerOp::Fsand: return std::format("fsand(m, {}, 0x{:x});", l.ft, l.immediate);
        case VuLowerOp::Fsor: return std::format("fsor(m, {}, 0x{:x});", l.ft, l.immediate);
        case VuLowerOp::Fsset: return std::format("fsset(m, 0x{:x});", l.immediate);
        case VuLowerOp::Fseq: return std::format("fseq(m, {}, 0x{:x});", l.ft, l.immediate);
        case VuLowerOp::Fmand: return std::format("fmand(m, {}, {});", l.ft, l.fs);
        case VuLowerOp::Fmor: return std::format("fmor(m, {}, {});", l.ft, l.fs);
        case VuLowerOp::Fmeq: return std::format("fmeq(m, {}, {});", l.ft, l.fs);
        case VuLowerOp::Iaddiu:
            return std::format("iaddiu(m, {}, {}, 0x{:x});", l.ft, l.fs, l.immediate);
        case VuLowerOp::Isubiu:
            return std::format("isubiu(m, {}, {}, 0x{:x});", l.ft, l.fs, l.immediate);
        case VuLowerOp::Ilw:
            return std::format("ilw<{}>(m, 0x{:x}, {}, {}, {});",
                                   unit, l.dest, l.ft, l.fs, l.offset);
        case VuLowerOp::Isw:
            return std::format("isw<{}>(m, 0x{:x}, {}, {}, {});",
                                   unit, l.dest, l.ft, l.fs, l.offset);
        case VuLowerOp::B: return std::format("b(m, 0x{:x}, {});", pc, l.offset);
        case VuLowerOp::Bal: return std::format("bal(m, 0x{:x}, {}, {});", pc, l.ft, l.offset);
        case VuLowerOp::Jr: return std::format("jr(m, {});", l.fs);
        case VuLowerOp::Jalr: return std::format("jalr(m, 0x{:x}, {}, {});", pc, l.ft, l.fs);
        case VuLowerOp::Ibeq:
            return std::format("ibeq(m, 0x{:x}, {}, {}, {});", pc, l.ft, l.fs, l.offset);
        case VuLowerOp::Ibne:
            return std::format("ibne(m, 0x{:x}, {}, {}, {});", pc, l.ft, l.fs, l.offset);
        case VuLowerOp::Ibltz: return std::format("ibltz(m, 0x{:x}, {}, {});", pc, l.fs, l.offset);
        case VuLowerOp::Ibgtz: return std::format("ibgtz(m, 0x{:x}, {}, {});", pc, l.fs, l.offset);
        case VuLowerOp::Iblez: return std::format("iblez(m, 0x{:x}, {}, {});", pc, l.fs, l.offset);
        case VuLowerOp::Ibgez: return std::format("ibgez(m, 0x{:x}, {}, {});", pc, l.fs, l.offset);
    }
    return {};
}

// Only the reads this pair makes are waited on, and only the writes it makes
// are marked.
std::string emitStall(const VuPairTiming& timing) {
    std::vector<std::string> terms;
    for (const VuLanes& read : {timing.upperReads[0], timing.upperReads[1], timing.lowerReads[0],
                                timing.lowerReads[1]}) {
        if (read.vf != 0 && read.lanes != 0) {
            terms.push_back(std::format("getLanesReady(m, {}, 0x{:x})", read.vf, read.lanes));
        }
    }
    for (u32 i = 0; i < timing.integerReadCount; ++i) {
        terms.push_back(std::format("getIntegerReady(m, {})", timing.integerReads[i]));
    }
    if (timing.waitsForDivider) {
        terms.push_back("getDividerFree(m)");
    }
    if (timing.waitsForFunction) {
        terms.push_back("getFunctionFree(m)");
    }
    if (terms.empty()) {
        return {};
    }
    std::string list = terms[0];
    for (usize i = 1; i < terms.size(); ++i) {
        list += ", " + terms[i];
    }
    return terms.size() == 1 ? std::format("    stallUntil(m, {});\n", list)
                             : std::format("    stallUntil(m, std::max({{{}}}));\n", list);
}

std::string emitMarks(const VuPairTiming& timing) {
    std::string marks;
    for (const VuLanes& write : {timing.upperWrite, timing.lowerWrite}) {
        if (write.vf != 0 && write.lanes != 0) {
            marks += std::format("    markLanes(m, {}, 0x{:x});\n", write.vf, write.lanes);
        }
    }
    if (timing.loadsInteger) {
        marks += std::format("    markIntegerLoad(m, {});\n", timing.integerLoad);
    }
    return marks;
}

// The same order a step runs a pair in. The lower op sees the register the
// upper one writes as it was before the pair, which only matters when the
// lower op touches that register at all.
std::string emitPair(u32 pc, u32 upperWord, u32 lowerWord, const VuPairTiming& checks,
                     bool inOrder, bool records, bool fast, u32 unit) {
    const VuPairTiming timing = getPairTiming(upperWord, lowerWord);
    const VuUpper decodedUpper = decodeUpper(upperWord);
    const VuLower decodedLower = decodeLower(upperWord, lowerWord);
    const std::string upper = emitUpper(decodedUpper, fast);
    const std::string lower = emitLower(pc, decodedLower, fast, unit);
    const std::string upperText = std::format("{:04x}  {}", pc, disassembleUpper(upperWord));
    const std::string lowerText = "      " + disassembleLower(pc, upperWord, lowerWord);
    const u32 target = getUpperDestination(upperWord);

    std::string text = emitStall(checks);
    if (!inOrder) {
        text += "    beginPair(m);\n";
    }
    if ((upperWord & kVuEndBit) != 0) {
        text += "    markEnding(m);\n";
    }
    if (target != 0 && touchesRegister(timing, target)) {
        const u32 dest = (upperWord >> 21) & 0xFu;
        text += std::format("    {{\n        const Reg128 upperBefore = s.vf[{}];\n", target);
        text += annotate("        ", upper, upperText);
        text += std::format("        const Reg128 upperResult = s.vf[{}];\n", target);
        text += std::format("        s.vf[{}] = upperBefore;\n", target);
        text += annotate("        ", lower, lowerText);
        if (dest == 0xFu) {
            text += std::format("        s.vf[{}] = upperResult;\n", target);
        }
        for (s32 i = 0; i < 4 && dest != 0xFu; ++i) {
            if (Vu::writes(dest, i)) {
                text += std::format("        s.vf[{}].setWord({}, upperResult.word({}));\n",
                                    target, i, i);
            }
        }
        text += "    }\n";
    } else {
        text += annotate("    ", upper, upperText);
        text += annotate("    ", lower, lowerText);
    }
    text += emitMarks(checks);
    if (records) {
        text += "    recordFlags(m);\n";
    }
    return text;
}

// Whether the pair reads the cycle count or a result that lands on one; in
// between such pairs the count can advance several cycles at once. An integer
// field past 15 reaches the control registers, Q and P among them.
bool readsCycle(const VuUpper& upper, const VuLower& lower, const VuPairTiming& checks,
                bool records) {
    if (records || !emitStall(checks).empty() || !emitMarks(checks).empty() ||
        upper.source == VuSource::Q || readsFlags(lower)) {
        return true;
    }
    switch (lower.op) {
        case VuLowerOp::Div:
        case VuLowerOp::Sqrt:
        case VuLowerOp::Rsqrt:
        case VuLowerOp::Function:
        case VuLowerOp::Mfp:
            return true;
        case VuLowerOp::Immediate:
        case VuLowerOp::None:
        case VuLowerOp::Move:
        case VuLowerOp::Mr32:
        case VuLowerOp::Rinit:
        case VuLowerOp::Rxor:
        case VuLowerOp::Rnext:
        case VuLowerOp::Rget:
            return false;
        case VuLowerOp::Lq:
        case VuLowerOp::Lqi:
        case VuLowerOp::Lqd:
        case VuLowerOp::Mfir:
            return lower.fs >= 16;
        case VuLowerOp::Sq:
        case VuLowerOp::Sqi:
        case VuLowerOp::Sqd:
        case VuLowerOp::Mtir:
            return lower.ft >= 16;
        default:
            return lower.ft >= 16 || lower.fs >= 16 || lower.fd >= 16;
    }
}

// How a translated function is entered and left. The one a program starts in
// covers what ran and hands anything else to its full translation, which takes
// over at the pair control left at.
struct ProgramForm {
    std::string name;
    u32 unit = 0;
    bool fast = false;
    // Takes over where control leaves these pairs; the interpreter when empty.
    std::string fallback;
    // Pairs another function enters this one at, through a switch on the pc.
    std::set<u32> entries;
};

std::string emitProgram(const Discovery& discovery, u32 start, const ProgramForm& form,
                        std::set<u32>& exits) {
    const std::set<u32>& included = discovery.getIncluded();
    const u32 count = discovery.getCount();
    const std::vector<u32> order(included.begin(), included.end());
    const PairFlow flow(discovery, start);
    const std::set<u32> afterEnd = getPairsAfterEnd(discovery);
    const std::set<u32> liveRecords = getLiveRecords(discovery, flow);
    const auto leave = [&](std::string_view pc) {
        return form.fallback.empty() ? std::format("return resume(m, {});", pc)
                                     : std::format("return {}(m, {});", form.fallback, pc);
    };

    // A label is only written where something jumps to it.
    std::set<u32> labelled = getLabels(discovery, order, start);
    labelled.insert(form.entries.begin(), form.entries.end());
    std::vector<std::string> pieces;
    // Cycles the pairs in order have not added yet. The counter is only read
    // where a label or an exit follows, so it is written there too.
    u32 pending = 0;
    for (usize i = 0; i < order.size(); ++i) {
        const u32 pc = order[i];
        const Pair pair = discovery.getPair(pc);
        const bool inOrder = !discovery.hasBranchInSlot() && !isDispatched(discovery, pc) &&
                             !pair.ends && !afterEnd.contains(pc);
        const VuPairTiming checks = pruneTiming(flow, pc);
        const bool records = liveRecords.contains(pc);
        const u32 next = (pc + 1) % count;
        std::string piece;
        if (pending != 0 && (!inOrder || readsCycle(decodeUpper(pair.upper),
                                                    decodeLower(pair.upper, pair.lower),
                                                    checks, records))) {
            piece += std::format("    advanceBy(m, {});\n", pending);
            pending = 0;
        }
        piece += emitPair(pc, pair.upper, pair.lower, checks, inOrder, records, form.fast,
                          form.unit);
        if (inOrder) {
            ++pending;
            const bool continues = included.contains(next) && i + 1 < order.size() &&
                                   order[i + 1] == next && !labelled.contains(next);
            if (!continues) {
                piece += std::format("    advanceBy(m, {});\n    m.programCounter = 0x{:04x};\n",
                                     pending, pc + 1);
                pending = 0;
            }
        } else {
            piece += std::format("    if (endPair(m, 0x{:04x})) return finish(m);\n", pc);
        }
        if (isDispatched(discovery, pc)) {
            // The counter is left unwrapped, the way the interpreter keeps it.
            piece += std::format("    switch (m.programCounter % 0x{:x}) {{\n", count);
            for (const u32 successor : getSuccessors(discovery, pc)) {
                piece += std::format("        case 0x{:04x}: goto {};\n", successor,
                                     label(successor));
            }
            const u32 site = (pc + count - 1) % count;
            const Pair branch = discovery.getPair(site);
            if (branch.transfer == VuTransfer::Indirect) {
                piece += std::format(
                    "        default: return resumeJump(m, 0x{:04x}, m.programCounter);\n    }}\n",
                    site);
            } else {
                for (const u32 target : discovery.getTargets(site, branch)) {
                    if (!included.contains(target)) {
                        exits.insert(target);
                    }
                }
                if (!included.contains(next)) {
                    exits.insert(next);
                }
                piece += std::format("        default: {}\n    }}\n", leave("m.programCounter"));
            }
        } else if (!included.contains(next)) {
            exits.insert(next);
            piece += std::format("    {}\n", leave(std::format("0x{:04x}", next)));
        } else if (i + 1 == order.size() || order[i + 1] != next) {
            // The set wraps, and the start need not be the lowest pair.
            piece += std::format("    goto {};\n", label(next));
        }
        pieces.push_back(std::move(piece));
    }

    // A jump back is where a program that never ends would spin, so the check
    // for a runaway goes where jumps land.
    std::string body;
    if (form.entries.empty()) {
        body = std::format(
            "VuMicro::Result {}(VuMicro& m) {{\n    Vu& s = m.state;\n    u32 steps = 0;\n"
            "    goto {};\n",
            form.name, label(start));
    } else {
        body = std::format(
            "VuMicro::Result {}(VuMicro& m, u32 pc) {{\n    Vu& s = m.state;\n    u32 steps = 0;\n"
            "    switch (pc % 0x{:x}) {{\n",
            form.name, count);
        for (const u32 entry : form.entries) {
            body += std::format("        case 0x{:04x}: goto {};\n", entry, label(entry));
        }
        body += "        default: return resume(m, pc);\n    }\n";
    }
    for (usize i = 0; i < order.size(); ++i) {
        if (labelled.contains(order[i])) {
            body += std::format(
                "\n{}:\n    if (++steps >= VuMicro::kRunawayStep) return stop(m);\n",
                label(order[i]));
        }
        body += pieces[i];
    }
    body += "}\n\n";
    return body;
}

// The program a record's start runs, in the exact mode or the fast one.
ProgramSource translateRecord(const VuProgramRecord& record, bool fast) {
    ProgramSource program;
    program.fast = fast;
    program.unit = record.unit;
    program.start = record.start;
    program.ranges = record.getRanges();
    program.hash = hashVuPairs(record.code, program.ranges);
    program.name = std::format("vu{}_{:016x}_{:04x}{}", record.unit, program.hash, program.start,
                               fast ? "_fast" : "");
    const Discovery full(record, record.reached);
    const ProgramForm alone{program.name, record.unit, fast, {}, {}};
    std::set<u32> exits;
    // A record that never ran its start has nothing to specialize.
    if (!record.covered[record.start]) {
        program.body = emitProgram(full, record.start, alone, exits);
        return program;
    }
    const Discovery ran(record, record.covered);
    const std::string fallback = program.name + "_full";
    const ProgramForm specialized{program.name, record.unit, fast, fallback, {}};
    program.body = emitProgram(ran, record.start, specialized, exits);
    std::set<u32> entries;
    for (const u32 exit : exits) {
        if (record.reached[exit]) {
            entries.insert(exit);
        }
    }
    if (entries.empty()) {
        // What ran is everything the start reaches.
        program.body = emitProgram(ran, record.start, alone, exits);
    } else {
        std::set<u32> unused;
        const ProgramForm complete{fallback, record.unit, fast, {}, entries};
        program.body += emitProgram(full, record.start, complete, unused);
        program.fallback = fallback;
    }
    return program;
}

std::string emitTable(std::string_view name, const std::vector<ProgramSource>& programs,
                      bool fast) {
    std::string table =
        std::format("\nextern const std::span<const TranslatedVuProgram> {};\n", name);
    std::vector<const ProgramSource*> chosen;
    for (const ProgramSource& program : programs) {
        if (program.fast == fast) {
            chosen.push_back(&program);
        }
    }
    // An empty log still builds, with nothing to look up.
    if (chosen.empty()) {
        return table + std::format("const std::span<const TranslatedVuProgram> {};\n", name);
    }
    table += std::format("static const TranslatedVuProgram {}Table[] = {{\n", name);
    for (const ProgramSource* program : chosen) {
        table += std::format("    {{{}, 0x{:x}, {}_pairs, 0x{:016x}, micro::{}}},\n", program->unit,
                             program->start, program->name, program->hash, program->name);
    }
    return table + std::format("}};\nconst std::span<const TranslatedVuProgram> {}{{{}Table}};\n",
                               name, name);
}

}  // namespace

TranslatedSource translateVuPrograms(const VuProgramLog& log, usize units,
                                     std::string_view declarationsHeader) {
    std::vector<ProgramSource> programs;
    for (const VuProgramRecord* record : log.getRecords()) {
        programs.push_back(translateRecord(*record, false));
        programs.push_back(translateRecord(*record, true));
    }
    // Two records can reach the same code; the first stands for both.
    std::sort(programs.begin(), programs.end(),
              [](const ProgramSource& a, const ProgramSource& b) { return a.name < b.name; });
    programs.erase(std::unique(programs.begin(), programs.end(),
                               [](const ProgramSource& a, const ProgramSource& b) {
                                   return a.name == b.name;
                               }),
                   programs.end());

    TranslatedSource out;
    out.declarations = "#pragma once\n\n#include \"vu_micro_handlers.h\"\n"
                       "#include \"vu_programs.h\"\n\n#include <algorithm>\n\n"
                       "namespace uee::micro {\n\n";
    for (const ProgramSource& program : programs) {
        out.declarations += std::format("VuMicro::Result {}(VuMicro& m);\n", program.name);
        if (!program.fallback.empty()) {
            out.declarations +=
                std::format("VuMicro::Result {}(VuMicro& m, u32 pc);\n", program.fallback);
        }
    }
    out.declarations += "\n}  // namespace uee::micro\n";

    std::string table = std::format("#include \"{}\"\n\nnamespace uee {{\n\n",
                                    declarationsHeader);
    for (const ProgramSource& program : programs) {
        table += std::format("static const VuPairRange {}_pairs[] = {{", program.name);
        for (const VuPairRange& range : program.ranges) {
            table += std::format("{{0x{:x}, {}}}, ", range.first, range.count);
        }
        table += "};\n";
    }
    table += emitTable("translatedVuPrograms", programs, false);
    table += emitTable("translatedFastVuPrograms", programs, true);
    table += "\n}  // namespace uee\n";
    out.units.push_back(std::move(table));

    // The table goes in the first file and the functions share the rest by
    // size. Each mode keeps files of its own: mixed, the fast copies spread the
    // exact ones through the binary and cost the exact mode 15%.
    const std::string open = std::format("#include \"{}\"\n\nnamespace uee::micro {{\n\n",
                                         declarationsHeader);
    const usize files = std::max<usize>(units, 3) - 1;
    const usize exactFiles = (files + 1) / 2;
    std::vector<std::string> bodies(files, open);
    for (const bool fast : {false, true}) {
        const usize first = fast ? exactFiles : 0;
        const usize count = fast ? files - exactFiles : exactFiles;
        usize total = 0;
        for (const ProgramSource& program : programs) {
            total += program.fast == fast ? program.body.size() : 0;
        }
        usize written = 0;
        for (const ProgramSource& program : programs) {
            if (program.fast != fast) {
                continue;
            }
            const usize file = std::min(count - 1, written * count / std::max<usize>(total, 1));
            bodies[first + file] += program.body;
            written += program.body.size();
        }
    }
    for (std::string& body : bodies) {
        out.units.push_back(body + "}  // namespace uee::micro\n");
    }
    return out;
}

}  // namespace uee
