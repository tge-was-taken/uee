#include "vu_micro_handlers.h"

#include <algorithm>
#include <cmath>

#include "gif.h"
#include "memory.h"
#include "vu_handlers.h"
#include "vu_programs.h"

namespace uee {
namespace {
constexpr u32 kOne = 0x3F800000u;
constexpr u32 kSine[5] = {0x3F800000u, 0xBE2AAAA4u, 0x3C08873Eu, 0xB94FB21Fu, 0x362E9C14u};
constexpr u32 kArcTangent[8] = {0x3F7FFFF5u, 0xBEAAA61Cu, 0x3E4C40A6u, 0xBE0E6C63u,
                               0x3DC577DFu, 0xBD6501C4u, 0x3CB31652u, 0xBB84D7E7u};
constexpr u32 kQuarterTurn = 0x3F490FDAu;
constexpr u32 kExponential[6] = {0x3E7FFFA8u, 0x3D0007F4u, 0x3B29D3FFu,
                                0x3933E553u, 0x36B63510u, 0x353961ACu};

// The ratio runs away when there is nothing to divide by, and the unit answers
// with a right angle whose exponent the overflow left behind.
constexpr u32 kOverflowedRightAngle = 0x7FC90FD7u;

inline u32 multiply(u32 a, u32 b) {
    return single::mul(a, b).bits;
}

inline u32 accumulate(u32 accumulator, u32 a, u32 b) {
    return single::multiplyAdd(accumulator, a, b, false).bits;
}

inline u32 computeEfuQuotient(u32 a, u32 b) {
    if (single::widen(b) == 0.0) {
        return ((a ^ b) & single::kSignMask) | single::kLargest;
    }
    return single::div(a, b).bits;
}

inline u32 computeEfuRoot(u32 a) {
    return single::sqrt(a).bits;
}

inline u32 computeEfuInverseRoot(u32 a, u32 b) {
    return computeEfuQuotient(a, computeEfuRoot(b));
}

inline u32 sumSquares(u32 x, u32 y, u32 z) {
    return accumulate(accumulate(multiply(x, x), y, y), z, z);
}

inline u32 sumLanes(u32 x, u32 y, u32 z, u32 w) {
    return accumulate(
        accumulate(accumulate(multiply(x, kOne), y, kOne), z, kOne), w, kOne);
}

template <usize N>
u32 evaluateOddSeries(u32 x, u32 accumulator, const u32 (&coefficients)[N], usize terms) {
    const u32 square = multiply(x, x);
    u32 odd = multiply(square, x);
    for (usize i = 1; i < terms; ++i) {
        const u32 next = multiply(odd, square);
        accumulator = accumulate(accumulator, odd, coefficients[i]);
        odd = next;
    }
    return accumulate(accumulator, odd, coefficients[terms]);
}

inline u32 computeSine(u32 x) {
    return evaluateOddSeries(x, multiply(x, kSine[0]), kSine, 4);
}

inline u32 computeArcTangentOfRatio(u32 x) {
    const u32 square = multiply(x, x);
    u32 accumulator = multiply(kArcTangent[0], x);
    u32 odd = multiply(square, x);
    for (usize i = 1; i < 7; ++i) {
        const u32 next = multiply(odd, square);
        accumulator = accumulate(accumulator, odd, kArcTangent[i]);
        odd = next;
    }
    accumulator = single::add(accumulator, kQuarterTurn).bits;
    return accumulate(accumulator, odd, kArcTangent[7]);
}

inline u32 computeArcTangent(u32 numerator, u32 denominator) {
    const u32 top = single::sub(numerator, denominator).bits;
    const u32 bottom = single::add(numerator, denominator).bits;
    if ((bottom & single::kMagnitudeMask) == 0) {
        return ((top ^ bottom) & single::kSignMask) | kOverflowedRightAngle;
    }
    return computeArcTangentOfRatio(computeEfuQuotient(top, bottom));
}

inline u32 computeExponential(u32 x) {
    u32 power = multiply(x, x);
    u32 accumulator = multiply(x, kExponential[0]);
    for (usize i = 1; i < 5; ++i) {
        const u32 next = multiply(power, x);
        accumulator = accumulate(accumulator, power, kExponential[i]);
        power = next;
    }
    accumulator = accumulate(accumulator, kOne, kOne);
    u32 value = accumulate(accumulator, power, kExponential[5]);
    value = multiply(value, value);
    value = multiply(value, value);
    return computeEfuQuotient(kOne, value);
}

constexpr u32 kPath1 = 1;

u64 readDouble(std::span<const u8> bytes, u32 offset) {
    u64 value;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

}  // namespace

namespace micro {

// The unit keeps working after the program stops, so the result lands whether
// or not anything is left to read it.
void syncPipelines(VuMicro& m) {
    m.state.syncFlags();
    m.state.vi[Vu::kClip] = m.state.clip;
    m.quotient.sync(m.state.vi[Vu::kQ]);
    m.function.sync(m.state.vi[Vu::kP]);
    m.landingCycle = ~0u;
}

void landResults(VuMicro& m) {
    m.quotient.land(m.cycle, m.state.vi[Vu::kQ]);
    m.function.land(m.cycle, m.state.vi[Vu::kP]);
    m.landingCycle = std::min(m.quotient.getLandingCycle(), m.function.getLandingCycle());
}

u32 computeFunction(VuMicro& m, u32 function, u32 fs, u32 lane) {
    const Reg128 source = m.state.readVf(fs);
    const u32 x = source.word(0), y = source.word(1);
    const u32 z = source.word(2), w = source.word(3);
    const u32 scalar = readLane(m, fs, lane);
    const u32 square = sumSquares(x, y, z);

    switch (function) {
        case elementary::kEsadd: return square;
        case elementary::kErsadd: return computeEfuQuotient(kOne, square);
        case elementary::kEleng: return computeEfuRoot(square);
        case elementary::kErleng: return computeEfuInverseRoot(kOne, square);
        case elementary::kEatanxy: return computeArcTangent(y, x);
        case elementary::kEatanxz: return computeArcTangent(z, x);
        case elementary::kEsum: return sumLanes(x, y, z, w);
        case elementary::kEsqrt: return computeEfuRoot(scalar);
        case elementary::kErsqrt: return computeEfuInverseRoot(kOne, scalar);
        case elementary::kErcpr: return computeEfuQuotient(kOne, scalar);
        case elementary::kEsin: return computeSine(scalar);
        case elementary::kEatan: return computeArcTangent(scalar, kOne);
        case elementary::kEexp: return computeExponential(scalar);
        default: return 0;
    }
}

// How long each function takes to reach the result register, which a program
// has to leave between the op and the read.
u32 getFunctionLatency(u32 function) {
    switch (function) {
        case elementary::kEsadd: return 11;
        case elementary::kEsum: return 12;
        case elementary::kErcpr: return 12;
        case elementary::kEsqrt: return 12;
        case elementary::kErsadd: return 18;
        case elementary::kEleng: return 18;
        case elementary::kErsqrt: return 18;
        case elementary::kErleng: return 24;
        case elementary::kEsin: return 29;
        case elementary::kEexp: return 44;
        default: return 54;
    }
}

u32 takeQuotient(VuMicro& m, Vu::Quotient q) {
    m.state.setDivideFlags(q.invalid, q.byZero);
    return q.value;
}

void xtop(VuMicro& m, u32 ft) {
    m.state.writeVi(ft, m.memory.read32(getVifTopAddress(m.unit)));
}

void xitop(VuMicro& m, u32 ft) {
    m.state.writeVi(ft, m.memory.read32(getVifItopAddress(m.unit)));
}

// The packet names no length, so the transfer runs until the unit reports the
// tag that ended it.
void kickToGif(VuMicro& m, u32 quadword) {
    // Only the second unit has a path to the interface.
    if (m.gif == nullptr) {
        return;
    }
    const u32 quadwords = m.getDataSize() / 16u;
    m.gif->beginPath(kPath1);
    for (u32 sent = 0; sent < quadwords; ++sent) {
        const u32 offset = ((quadword + sent) % quadwords) * 16u;
        m.gif->writeQuadword(readDouble(m.data, offset), readDouble(m.data, offset + 8));
        if (!m.gif->isPacketOpen()) {
            break;
        }
    }
    m.gif->endPath();
}

VuMicro::Result resume(VuMicro& m, u32 pc) {
    m.programCounter = pc;
    for (u32 steps = 0; steps < VuMicro::kRunawayStep; ++steps) {
        if (m.step()) {
            return finish(m);
        }
    }
    return stop(m);
}

// The counter names where the program would have gone next, and wraps within
// the instruction memory like a fetch does.
VuMicro::Result finish(const VuMicro& m) {
    return {(m.programCounter + 1) % m.getInstructionCount(), true};
}

VuMicro::Result stop(VuMicro& m) {
    syncPipelines(m);
    return {m.programCounter, false};
}

}  // namespace micro

namespace {

using namespace micro;

void executeUpper(VuMicro& m, const VuUpper& u) {
    Vu& s = m.state;
    switch (u.op) {
        case VuUpperOp::None: return;
        case VuUpperOp::Add: vadd(s, u.source, u.dest, u.fd, u.fs, u.ft); return;
        case VuUpperOp::Sub: vsub(s, u.source, u.dest, u.fd, u.fs, u.ft); return;
        case VuUpperOp::Madd: vmadd(s, u.source, u.dest, u.fd, u.fs, u.ft); return;
        case VuUpperOp::Msub: vmsub(s, u.source, u.dest, u.fd, u.fs, u.ft); return;
        case VuUpperOp::Max: vmax(s, u.source, u.dest, u.fd, u.fs, u.ft); return;
        case VuUpperOp::Mini: vmini(s, u.source, u.dest, u.fd, u.fs, u.ft); return;
        case VuUpperOp::Mul: vmul(s, u.source, u.dest, u.fd, u.fs, u.ft); return;
        case VuUpperOp::Adda: vadda(s, u.source, u.dest, u.fs, u.ft); return;
        case VuUpperOp::Suba: vsuba(s, u.source, u.dest, u.fs, u.ft); return;
        case VuUpperOp::Madda: vmadda(s, u.source, u.dest, u.fs, u.ft); return;
        case VuUpperOp::Msuba: vmsuba(s, u.source, u.dest, u.fs, u.ft); return;
        case VuUpperOp::Mula: vmula(s, u.source, u.dest, u.fs, u.ft); return;
        case VuUpperOp::Opmsub: vopmsub(s, u.dest, u.fd, u.fs, u.ft); return;
        case VuUpperOp::Opmula: vopmula(s, u.dest, u.fs, u.ft); return;
        case VuUpperOp::Itof: vitof(s, u.dest, u.ft, u.fs, static_cast<s32>(u.shift)); return;
        case VuUpperOp::Ftoi: vftoi(s, u.dest, u.ft, u.fs, static_cast<s32>(u.shift)); return;
        case VuUpperOp::Abs: vabs(s, u.dest, u.ft, u.fs); return;
        case VuUpperOp::Clip: vclip(s, u.fs, u.ft); return;
        case VuUpperOp::Move: vmove(s, u.dest, u.ft, u.fs); return;
    }
}

void executeLower(VuMicro& m, u32 pc, const VuLower& l) {
    Vu& s = m.state;
    switch (l.op) {
        case VuLowerOp::None: return;
        case VuLowerOp::Immediate: setImmediate(m, l.immediate); return;
        case VuLowerOp::Move: vmove(s, l.dest, l.ft, l.fs); return;
        case VuLowerOp::Mr32: vmr32(s, l.dest, l.ft, l.fs); return;
        case VuLowerOp::Mfir: vmfir(s, l.dest, l.ft, l.fs); return;
        case VuLowerOp::Mtir: vmtir(s, l.ft, l.fs, static_cast<s32>(l.fsLane)); return;
        case VuLowerOp::Rinit: rinit(m, l.fs, l.fsLane); return;
        case VuLowerOp::Rxor: rxor(m, l.fs, l.fsLane); return;
        case VuLowerOp::Rnext: rnext(m, l.dest, l.ft); return;
        case VuLowerOp::Rget: vrget(s, l.dest, l.ft); return;
        case VuLowerOp::Mfp: mfp(m, l.dest, l.ft); return;
        case VuLowerOp::Xgkick: xgkick(m, l.fs); return;
        case VuLowerOp::Xtop: xtop(m, l.ft); return;
        case VuLowerOp::Xitop: xitop(m, l.ft); return;
        case VuLowerOp::Function: efu(m, l.function, l.fs, l.fsLane); return;
        case VuLowerOp::Div: div(m, l.fs, l.fsLane, l.ft, l.ftLane); return;
        case VuLowerOp::Sqrt: sqrt(m, l.ft, l.ftLane); return;
        case VuLowerOp::Rsqrt: rsqrt(m, l.fs, l.fsLane, l.ft, l.ftLane); return;
        case VuLowerOp::Ilwr: ilwr(m, l.dest, l.ft, l.fs); return;
        case VuLowerOp::Iswr: iswr(m, l.dest, l.ft, l.fs); return;
        case VuLowerOp::Lqi: lqi(m, l.dest, l.ft, l.fs); return;
        case VuLowerOp::Sqi: sqi(m, l.dest, l.fs, l.ft); return;
        case VuLowerOp::Lqd: lqd(m, l.dest, l.ft, l.fs); return;
        case VuLowerOp::Sqd: sqd(m, l.dest, l.fs, l.ft); return;
        case VuLowerOp::Iadd: viadd(s, l.fd, l.fs, l.ft); return;
        case VuLowerOp::Isub: visub(s, l.fd, l.fs, l.ft); return;
        case VuLowerOp::Iand: viand(s, l.fd, l.fs, l.ft); return;
        case VuLowerOp::Ior: vior(s, l.fd, l.fs, l.ft); return;
        case VuLowerOp::Iaddi: viaddi(s, l.ft, l.fs, l.offset); return;
        case VuLowerOp::Lq: lq(m, l.dest, l.ft, l.fs, l.offset); return;
        case VuLowerOp::Sq: sq(m, l.dest, l.fs, l.ft, l.offset); return;
        case VuLowerOp::Fcget: fcget(m, l.ft); return;
        case VuLowerOp::Fcset: fcset(m, l.immediate); return;
        case VuLowerOp::Fcand: fcand(m, l.immediate); return;
        case VuLowerOp::Fcor: fcor(m, l.immediate); return;
        case VuLowerOp::Fceq: fceq(m, l.immediate); return;
        case VuLowerOp::Fsand: fsand(m, l.ft, l.immediate); return;
        case VuLowerOp::Fsor: fsor(m, l.ft, l.immediate); return;
        case VuLowerOp::Fsset: fsset(m, l.immediate); return;
        case VuLowerOp::Fseq: fseq(m, l.ft, l.immediate); return;
        case VuLowerOp::Fmand: fmand(m, l.ft, l.fs); return;
        case VuLowerOp::Fmor: fmor(m, l.ft, l.fs); return;
        case VuLowerOp::Fmeq: fmeq(m, l.ft, l.fs); return;
        case VuLowerOp::Iaddiu: iaddiu(m, l.ft, l.fs, l.immediate); return;
        case VuLowerOp::Isubiu: isubiu(m, l.ft, l.fs, l.immediate); return;
        case VuLowerOp::Ilw: ilw(m, l.dest, l.ft, l.fs, l.offset); return;
        case VuLowerOp::Isw: isw(m, l.dest, l.ft, l.fs, l.offset); return;
        case VuLowerOp::B: b(m, pc, l.offset); return;
        case VuLowerOp::Bal: bal(m, pc, l.ft, l.offset); return;
        case VuLowerOp::Jr: jr(m, l.fs); return;
        case VuLowerOp::Jalr: jalr(m, pc, l.ft, l.fs); return;
        case VuLowerOp::Ibeq: ibeq(m, pc, l.ft, l.fs, l.offset); return;
        case VuLowerOp::Ibne: ibne(m, pc, l.ft, l.fs, l.offset); return;
        case VuLowerOp::Ibltz: ibltz(m, pc, l.fs, l.offset); return;
        case VuLowerOp::Ibgtz: ibgtz(m, pc, l.fs, l.offset); return;
        case VuLowerOp::Iblez: iblez(m, pc, l.fs, l.offset); return;
        case VuLowerOp::Ibgez: ibgez(m, pc, l.fs, l.offset); return;
    }
}

// The lower op reads what the upper one writes as it was before the pair; where
// both write a register, the upper op's lanes win.
void executeLowerBeside(VuMicro& m, u32 pc, u32 upperWord, const VuLower& lower,
                        const Reg128& upperBefore) {
    const u32 target = getUpperDestination(upperWord);
    if (target == 0) {
        executeLower(m, pc, lower);
        return;
    }
    const Reg128 upperResult = m.state.vf[target];
    m.state.vf[target] = upperBefore;
    executeLower(m, pc, lower);
    const u32 dest = (upperWord >> 21) & 0xFu;
    for (s32 i = 0; i < 4; ++i) {
        if (Vu::writes(dest, i)) {
            m.state.vf[target].setWord(i, upperResult.word(i));
        }
    }
}

u32 getReadyCycle(const VuMicro& m, const VuPairTiming& timing) {
    u32 ready = 0;
    for (const VuLanes& read : timing.upperReads) {
        ready = std::max(ready, getLanesReady(m, read.vf, read.lanes));
    }
    for (const VuLanes& read : timing.lowerReads) {
        ready = std::max(ready, getLanesReady(m, read.vf, read.lanes));
    }
    for (u32 i = 0; i < timing.integerReadCount; ++i) {
        ready = std::max(ready, getIntegerReady(m, timing.integerReads[i]));
    }
    if (timing.waitsForDivider) {
        ready = std::max(ready, getDividerFree(m));
    }
    if (timing.waitsForFunction) {
        ready = std::max(ready, getFunctionFree(m));
    }
    return ready;
}

void markWritten(VuMicro& m, const VuPairTiming& timing) {
    markLanes(m, timing.upperWrite.vf, timing.upperWrite.lanes);
    markLanes(m, timing.lowerWrite.vf, timing.lowerWrite.lanes);
    if (timing.loadsInteger) {
        markIntegerLoad(m, timing.integerLoad);
    }
}

bool runPair(VuMicro& m, u32 pc, u32 upperWord, u32 lowerWord) {
    const VuPairTiming timing = getPairTiming(upperWord, lowerWord);
    stallUntil(m, getReadyCycle(m, timing));
    beginPair(m);
    if ((upperWord & kVuEndBit) != 0) {
        markEnding(m);
    }
    const Reg128 upperBefore = m.state.vf[getUpperDestination(upperWord)];
    const VuUpper upper = decodeUpper(upperWord);
    executeUpper(m, upper);
    const VuLower lower = decodeLower(upperWord, lowerWord);
    if (lower.op == VuLowerOp::Immediate) {
        setImmediate(m, lower.immediate);
    } else {
        executeLowerBeside(m, pc, upperWord, lower, upperBefore);
    }
    markWritten(m, timing);
    if (changesFlags(upper, lower)) {
        recordFlags(m);
    }
    return endPair(m, pc);
}

}  // namespace

VuMicro::VuMicro(Vu& state, Memory& memory, u32 unit, Gif* gif)
    : state(state), memory(memory), gif(gif), unit(unit),
      instructions(memory.getRegion(unit == 0 ? kVu0MicroBase : kVu1MicroBase,
                                    unit == 0 ? kVu0MemorySize : kVu1MemorySize)),
      data(memory.getRegion(getDataBase(), getDataSize())) {}


VuMicro::Result VuMicro::run(u32 start) {
    begin(start);
    return micro::resume(*this, start);
}

void VuMicro::begin(u32 start) {
    programCounter = start;
    branching = false;
    taken = false;
    ending = false;
    endDelay = 1;
    const FlagSet seen{state.readSeenStatus(), state.readSeenMac(), state.vi[Vu::kClip]};
    std::fill(std::begin(flagHistory), std::end(flagHistory), FlagRecord{0, seen});
    flagNewest = 0;
    quotient = {};
    function = {};
    landingCycle = ~0u;
    cycle = 0;
    for (auto& lanes : laneReady) {
        std::fill(std::begin(lanes), std::end(lanes), 0u);
    }
    std::fill(std::begin(integerReady), std::end(integerReady), 0u);
}

bool VuMicro::step() {
    const u32 offset = (programCounter % getInstructionCount()) * 8;
    return runPair(*this, programCounter, micro::readWord(instructions, offset + 4),
                   micro::readWord(instructions, offset));
}

bool runMicroProgram(Vu& state, Memory& memory, Gif* gif, u32 unit, u32 start) {
    const VuMicro::Result result = state.programs != nullptr
                                       ? state.programs->run(state, memory, gif, unit, start)
                                       : VuMicro(state, memory, unit, gif).run(start);
    state.vi[Vu::kTpc] = result.counter;
    return result.ended;
}

}  // namespace uee
