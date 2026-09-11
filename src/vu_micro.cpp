#include "vu_micro.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <stdexcept>

#include "gif.h"
#include "memory.h"
#include "vu_handlers.h"

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

inline u32 efuDivide(u32 a, u32 b) {
    if (single::widen(b) == 0.0) {
        return ((a ^ b) & single::kSignMask) | single::kLargest;
    }
    return single::div(a, b).bits;
}

inline u32 efuRoot(u32 a) {
    return single::sqrt(a).bits;
}

inline u32 efuInverseRoot(u32 a, u32 b) {
    return efuDivide(a, efuRoot(b));
}

inline u32 sumOfSquares(u32 x, u32 y, u32 z) {
    return accumulate(accumulate(multiply(x, x), y, y), z, z);
}

inline u32 sum(u32 x, u32 y, u32 z, u32 w) {
    return accumulate(
        accumulate(accumulate(multiply(x, kOne), y, kOne), z, kOne), w, kOne);
}

template <usize N>
u32 oddSeries(u32 x, u32 accumulator, const u32 (&coefficients)[N], usize terms) {
    const u32 square = multiply(x, x);
    u32 odd = multiply(square, x);
    for (usize i = 1; i < terms; ++i) {
        const u32 next = multiply(odd, square);
        accumulator = accumulate(accumulator, odd, coefficients[i]);
        odd = next;
    }
    return accumulate(accumulator, odd, coefficients[terms]);
}

inline u32 sine(u32 x) {
    return oddSeries(x, multiply(x, kSine[0]), kSine, 4);
}

inline u32 arcTangentOfRatio(u32 x) {
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

inline u32 arcTangent(u32 numerator, u32 denominator) {
    const u32 top = single::sub(numerator, denominator).bits;
    const u32 bottom = single::add(numerator, denominator).bits;
    if ((bottom & single::kMagnitudeMask) == 0) {
        return ((top ^ bottom) & single::kSignMask) | kOverflowedRightAngle;
    }
    return arcTangentOfRatio(efuDivide(top, bottom));
}

inline u32 exponential(u32 x) {
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
    return efuDivide(kOne, value);
}


constexpr u32 kEndBit = 1u << 30;
constexpr u32 kImmediateBit = 1u << 31;

constexpr bool isRegisterForm(u32 code) {
    return (code & 0x80000000u) != 0;
}

constexpr u32 destOf(u32 code) { return (code >> 21) & 0xFu; }

// The status ops carry a twelve bit immediate with its top bit ten places up
// from the rest.
constexpr u32 statusImmediate(u32 code) {
    return (code & 0x7FFu) | ((code >> 10) & 0x800u);
}
constexpr u32 ftOf(u32 code) { return (code >> 16) & 0x1Fu; }
constexpr u32 fsOf(u32 code) { return (code >> 11) & 0x1Fu; }
constexpr u32 fdOf(u32 code) { return (code >> 6) & 0x1Fu; }
constexpr u32 broadcastOf(u32 code) { return code & 3u; }

// The vector register an upper instruction writes, or zero for one that writes
// only the accumulator, the clip flags or nothing.
constexpr u32 upperDestination(u32 code) {
    if ((code & 0x3Fu) < 0x30u) {
        return fdOf(code);
    }
    if ((code & 0x3Cu) != 0x3Cu) {
        return 0;
    }
    switch (((code >> 6) & 0x1Fu) << 6 | (code & 0x3Fu)) {
        case 0x13C: case 0x13D: case 0x13E: case 0x13F:
        case 0x17C: case 0x17D: case 0x17E: case 0x17F:
        case 0x1FD:
            return ftOf(code);
        default:
            return 0;
    }
}

constexpr u32 immediate15(u32 code) {
    return (code & 0x7FFu) | ((code >> 10) & 0x7800u);
}

constexpr s32 signed11(u32 code) {
    return static_cast<s32>((code & 0x7FFu) << 21) >> 21;
}

constexpr s32 signed5(u32 code) {
    return static_cast<s32>(((code >> 6) & 0x1Fu) << 27) >> 27;
}

constexpr VuSource broadcastSource(u32 code) {
    switch (broadcastOf(code)) {
        case 0: return VuSource::X;
        case 1: return VuSource::Y;
        case 2: return VuSource::Z;
        default: return VuSource::W;
    }
}

namespace lower {
constexpr u32 kIadd = 0x30, kIsub = 0x31, kIaddi = 0x32, kIand = 0x34, kIor = 0x35;
constexpr u32 kIlw = 0x04, kIsw = 0x05;
constexpr u32 kB = 0x20, kBal = 0x21, kJr = 0x24, kJalr = 0x25;
constexpr u32 kIbeq = 0x28, kIbne = 0x29;
constexpr u32 kIbltz = 0x2C, kIbgtz = 0x2D, kIblez = 0x2E, kIbgez = 0x2F;
constexpr u32 kIaddiu = 0x08, kIsubiu = 0x09;
constexpr u32 kFceq = 0x10, kFcset = 0x11, kFcand = 0x12, kFcor = 0x13;
constexpr u32 kFseq = 0x14, kFsset = 0x15, kFsand = 0x16, kFsor = 0x17;
constexpr u32 kFmeq = 0x18, kFmand = 0x1A, kFmor = 0x1B, kFcget = 0x1C;
constexpr u32 kLq = 0x00, kSq = 0x01;
constexpr u32 kMove = 0x33C, kMr32 = 0x33D;
constexpr u32 kLqi = 0x37C, kSqi = 0x37D, kLqd = 0x37E, kSqd = 0x37F;
constexpr u32 kMtir = 0x3FC, kMfir = 0x3FD, kIlwr = 0x3FE, kIswr = 0x3FF;
constexpr u32 kRnext = 0x43C, kRget = 0x43D, kRinit = 0x43E, kRxor = 0x43F;
constexpr u32 kDiv = 0x3BC, kSqrt = 0x3BD, kRsqrt = 0x3BE, kWaitq = 0x3BF;
constexpr u32 kMfp = 0x67C;
constexpr u32 kEsadd = 0x73C, kErsadd = 0x73D, kEleng = 0x73E, kErleng = 0x73F;
constexpr u32 kEatanxy = 0x77C, kEatanxz = 0x77D, kEsum = 0x77E;
constexpr u32 kEsqrt = 0x7BC, kErsqrt = 0x7BD, kErcpr = 0x7BE, kWaitp = 0x7BF;
constexpr u32 kEsin = 0x7FC, kEatan = 0x7FD, kEexp = 0x7FE;
constexpr u32 kXgkick = 0x6FC, kXtop = 0x6BC, kXitop = 0x6BD;
}  // namespace lower

constexpr u32 kPath1 = 1;

// The hardware unit runs beside the processor, so a program with no end bit
// spins there forever while the guest carries on. Running it here has to stop
// somewhere, and a program that means to end does so long before this.
constexpr u32 kRunawayStep = 1'000'000;

constexpr u32 kVifRegisters[2] = {0x10003800, 0x10003C00};
constexpr u32 kItop = 0xD0, kTop = 0xE0;

}  // namespace

u32 VuMicro::instructionBase() const { return mUnit == 0 ? kVu0MicroBase : kVu1MicroBase; }
u32 VuMicro::instructionCount() const {
    return (mUnit == 0 ? kVu0MemorySize : kVu1MemorySize) / 8;
}
u32 VuMicro::dataBase() const { return mUnit == 0 ? kVu0DataBase : kVu1DataBase; }
u32 VuMicro::dataSize() const { return mUnit == 0 ? kVu0MemorySize : kVu1MemorySize; }

void VuMicro::executeUpper(u32 code) {
    const u32 dest = destOf(code);
    const u32 fd = fdOf(code), fs = fsOf(code), ft = ftOf(code);
    const u32 opcode = code & 0x3Fu;

    switch (opcode >> 2) {
        case 0x0: vadd(mState, broadcastSource(code), dest, fd, fs, ft); return;
        case 0x1: vsub(mState, broadcastSource(code), dest, fd, fs, ft); return;
        case 0x2: vmadd(mState, broadcastSource(code), dest, fd, fs, ft); return;
        case 0x3: vmsub(mState, broadcastSource(code), dest, fd, fs, ft); return;
        case 0x4: vmax(mState, broadcastSource(code), dest, fd, fs, ft); return;
        case 0x5: vmini(mState, broadcastSource(code), dest, fd, fs, ft); return;
        case 0x6: vmul(mState, broadcastSource(code), dest, fd, fs, ft); return;
        default: break;
    }
    switch (opcode) {
        case 0x1C: vmul(mState, VuSource::Q, dest, fd, fs, ft); return;
        case 0x1D: vmax(mState, VuSource::I, dest, fd, fs, ft); return;
        case 0x1E: vmul(mState, VuSource::I, dest, fd, fs, ft); return;
        case 0x1F: vmini(mState, VuSource::I, dest, fd, fs, ft); return;
        case 0x20: vadd(mState, VuSource::Q, dest, fd, fs, ft); return;
        case 0x21: vmadd(mState, VuSource::Q, dest, fd, fs, ft); return;
        case 0x22: vadd(mState, VuSource::I, dest, fd, fs, ft); return;
        case 0x23: vmadd(mState, VuSource::I, dest, fd, fs, ft); return;
        case 0x24: vsub(mState, VuSource::Q, dest, fd, fs, ft); return;
        case 0x25: vmsub(mState, VuSource::Q, dest, fd, fs, ft); return;
        case 0x26: vsub(mState, VuSource::I, dest, fd, fs, ft); return;
        case 0x27: vmsub(mState, VuSource::I, dest, fd, fs, ft); return;
        case 0x28: vadd(mState, VuSource::Vt, dest, fd, fs, ft); return;
        case 0x29: vmadd(mState, VuSource::Vt, dest, fd, fs, ft); return;
        case 0x2A: vmul(mState, VuSource::Vt, dest, fd, fs, ft); return;
        case 0x2B: vmax(mState, VuSource::Vt, dest, fd, fs, ft); return;
        case 0x2C: vsub(mState, VuSource::Vt, dest, fd, fs, ft); return;
        case 0x2D: vmsub(mState, VuSource::Vt, dest, fd, fs, ft); return;
        case 0x2E: vopmsub(mState, dest, fd, fs, ft); return;
        case 0x2F: vmini(mState, VuSource::Vt, dest, fd, fs, ft); return;
        default: break;
    }

    switch (((code >> 6) & 0x1Fu) << 6 | (code & 0x3Fu)) {
        case 0x03C: vadda(mState, VuSource::X, dest, fs, ft); return;
        case 0x03D: vadda(mState, VuSource::Y, dest, fs, ft); return;
        case 0x03E: vadda(mState, VuSource::Z, dest, fs, ft); return;
        case 0x03F: vadda(mState, VuSource::W, dest, fs, ft); return;
        case 0x07C: vsuba(mState, VuSource::X, dest, fs, ft); return;
        case 0x07D: vsuba(mState, VuSource::Y, dest, fs, ft); return;
        case 0x07E: vsuba(mState, VuSource::Z, dest, fs, ft); return;
        case 0x07F: vsuba(mState, VuSource::W, dest, fs, ft); return;
        case 0x0BC: vmadda(mState, VuSource::X, dest, fs, ft); return;
        case 0x0BD: vmadda(mState, VuSource::Y, dest, fs, ft); return;
        case 0x0BE: vmadda(mState, VuSource::Z, dest, fs, ft); return;
        case 0x0BF: vmadda(mState, VuSource::W, dest, fs, ft); return;
        case 0x0FC: vmsuba(mState, VuSource::X, dest, fs, ft); return;
        case 0x0FD: vmsuba(mState, VuSource::Y, dest, fs, ft); return;
        case 0x0FE: vmsuba(mState, VuSource::Z, dest, fs, ft); return;
        case 0x0FF: vmsuba(mState, VuSource::W, dest, fs, ft); return;
        case 0x1BC: vmula(mState, VuSource::X, dest, fs, ft); return;
        case 0x1BD: vmula(mState, VuSource::Y, dest, fs, ft); return;
        case 0x1BE: vmula(mState, VuSource::Z, dest, fs, ft); return;
        case 0x1BF: vmula(mState, VuSource::W, dest, fs, ft); return;
        case 0x1FC: vmula(mState, VuSource::Q, dest, fs, ft); return;
        case 0x1FE: vmula(mState, VuSource::I, dest, fs, ft); return;
        case 0x23C: vadda(mState, VuSource::Q, dest, fs, ft); return;
        case 0x23D: vmadda(mState, VuSource::Q, dest, fs, ft); return;
        case 0x23E: vadda(mState, VuSource::I, dest, fs, ft); return;
        case 0x23F: vmadda(mState, VuSource::I, dest, fs, ft); return;
        case 0x27C: vsuba(mState, VuSource::Q, dest, fs, ft); return;
        case 0x27D: vmsuba(mState, VuSource::Q, dest, fs, ft); return;
        case 0x27E: vsuba(mState, VuSource::I, dest, fs, ft); return;
        case 0x27F: vmsuba(mState, VuSource::I, dest, fs, ft); return;
        case 0x2BC: vadda(mState, VuSource::Vt, dest, fs, ft); return;
        case 0x2BD: vmadda(mState, VuSource::Vt, dest, fs, ft); return;
        case 0x2BE: vmula(mState, VuSource::Vt, dest, fs, ft); return;
        case 0x2FC: vsuba(mState, VuSource::Vt, dest, fs, ft); return;
        case 0x2FD: vmsuba(mState, VuSource::Vt, dest, fs, ft); return;
        case 0x13C: vitof(mState, dest, ft, fs, 0); return;
        case 0x13D: vitof(mState, dest, ft, fs, 4); return;
        case 0x13E: vitof(mState, dest, ft, fs, 12); return;
        case 0x13F: vitof(mState, dest, ft, fs, 15); return;
        case 0x17C: vftoi(mState, dest, ft, fs, 0); return;
        case 0x17D: vftoi(mState, dest, ft, fs, 4); return;
        case 0x17E: vftoi(mState, dest, ft, fs, 12); return;
        case 0x17F: vftoi(mState, dest, ft, fs, 15); return;
        case 0x1FD: vabs(mState, dest, ft, fs); return;
        case 0x1FF: vclip(mState, fs, ft); return;
        case 0x2FE: vopmula(mState, dest, fs, ft); return;
        case 0x33C: vmove(mState, dest, ft, fs); return;
        default: return;
    }
}

u32 VuMicro::operandsReadyAt(u32 upper, u32 lower) const {
    u32 ready = std::max(mWriteReady[fsOf(upper)], mWriteReady[ftOf(upper)]);
    if ((upper & kImmediateBit) != 0) {
        return ready;
    }
    const u32 fs = mWriteReady[fsOf(lower)], ft = mWriteReady[ftOf(lower)];
    if (!isRegisterForm(lower)) {
        if (((lower >> 25) & 0x7Fu) == lower::kSq) {
            ready = std::max(ready, fs);
        }
        return ready;
    }
    if ((lower & 0x3Cu) != 0x3Cu) {
        return ready;
    }
    switch (((lower >> 6) & 0x1Fu) << 6 | (lower & 0x3Fu)) {
        case lower::kDiv:
        case lower::kRsqrt:
            return std::max(ready, std::max(fs, ft));
        case lower::kSqrt:
            return std::max(ready, ft);
        case lower::kMove:
        case lower::kMr32:
        case lower::kMtir:
        case lower::kSqi:
        case lower::kSqd:
        case lower::kRinit:
        case lower::kRxor:
            return std::max(ready, fs);
        default:
            return ready;
    }
}

// The instruction that set a flag keeps moving while the one waiting on it
// stalls, so the clip flags land after the same number of cycles either way.
void VuMicro::stallUntil(u32 cycle) {
    while (mCycle < cycle) {
        advancePipelines();
        mState.vi[Vu::kClip] = mClipDelayLine[0];
        for (u32 i = 0; i + 1 < kClipDelay; ++i) {
            mClipDelayLine[i] = mClipDelayLine[i + 1];
        }
        mClipDelayLine[kClipDelay - 1] = mState.clip;
        ++mCycle;
    }
}

// The unit keeps working after the program stops, so the result lands whether
// or not anything is left to read it.
void VuMicro::settlePipelines() {
    mState.settleFlags();
    mQuotient.settle(mState.vi[Vu::kQ]);
    mFunction.settle(mState.vi[Vu::kP]);
}

void VuMicro::advancePipelines() {
    mQuotient.advance(mState.vi[Vu::kQ]);
    mFunction.advance(mState.vi[Vu::kP]);
}

u32 VuMicro::elementaryFunction(u32 wide, u32 code, u32 vs) {
    const Reg128 source = mState.readVf(vs);
    const u32 x = source.word(0), y = source.word(1);
    const u32 z = source.word(2), w = source.word(3);
    const u32 scalar = sourceLane(code, vs);
    const u32 square = sumOfSquares(x, y, z);

    switch (wide) {
        case lower::kEsadd: return square;
        case lower::kErsadd: return efuDivide(kOne, square);
        case lower::kEleng: return efuRoot(square);
        case lower::kErleng: return efuInverseRoot(kOne, square);
        case lower::kEatanxy: return arcTangent(y, x);
        case lower::kEatanxz: return arcTangent(z, x);
        case lower::kEsum: return sum(x, y, z, w);
        case lower::kEsqrt: return efuRoot(scalar);
        case lower::kErsqrt: return efuInverseRoot(kOne, scalar);
        case lower::kErcpr: return efuDivide(kOne, scalar);
        case lower::kEsin: return sine(scalar);
        case lower::kEatan: return arcTangent(scalar, kOne);
        case lower::kEexp: return exponential(scalar);
        default: return 0;
    }
}

// How long each function takes to reach the result register, which a program
// has to leave between the op and the read.
u32 VuMicro::functionLatency(u32 wide) {
    switch (wide) {
        case lower::kEsadd: return 11;
        case lower::kEsum: return 12;
        case lower::kErcpr: return 12;
        case lower::kEsqrt: return 12;
        case lower::kErsadd: return 18;
        case lower::kEleng: return 18;
        case lower::kErsqrt: return 18;
        case lower::kErleng: return 24;
        case lower::kEsin: return 29;
        case lower::kEexp: return 44;
        default: return 54;
    }
}

u32 VuMicro::quotientOf(u32 code, u32 vs, u32 vt) {
    return takeQuotient(divide(sourceLane(code, vs), targetLane(code, vt)));
}

u32 VuMicro::rootOf(u32 code, u32 vt) {
    return takeQuotient(root(targetLane(code, vt)));
}

u32 VuMicro::inverseRootOf(u32 code, u32 vs, u32 vt) {
    return takeQuotient(inverseRoot(sourceLane(code, vs), targetLane(code, vt)));
}

u32 VuMicro::takeQuotient(Vu::Quotient q) {
    mState.setDivideFlags(q.invalid, q.byZero);
    return q.value;
}

u32 VuMicro::targetLane(u32 code, u32 vf) const {
    return mState.readVf(vf).word(static_cast<s32>((code >> 23) & 3u));
}

u32 VuMicro::sourceLane(u32 code, u32 vf) const {
    return mState.readVf(vf).word(static_cast<s32>((code >> 21) & 3u));
}

void VuMicro::advanceRandom() {
    u32& value = mState.vi[Vu::kR];
    const u32 bit = ((value >> 4) ^ (value >> 22)) & 1u;
    value = ((value << 1) | bit) & 0x7FFFFFu;
}

// The first unit's four kilobytes repeat over the sixteen it decodes, and a
// pointer past the end of that reads nothing at all. The second unit fills its
// decode, so every pointer lands in memory and wraps there.
bool isAddressable(u32 unit, u32 quadword) {
    constexpr u32 kDecodedQuadwords = 0x400;
    if (unit != 0) {
        return true;
    }
    return static_cast<s16>(quadword) >= 0 &&
           (quadword & 0xFFFFu) < kDecodedQuadwords;
}

void VuMicro::loadQuadword(u32 dest, u32 vf, u32 quadword) {
    if (!isAddressable(mUnit, quadword)) {
        const u32 zero[4]{};
        mState.writeVf(vf, dest, zero);
        return;
    }
    const u32 address = dataBase() + (quadword * 16u) % dataSize();
    u32 lanes[4];
    for (s32 i = 0; i < 4; ++i) {
        lanes[i] = mMemory.read32(address + 4u * static_cast<u32>(i));
    }
    mState.writeVf(vf, dest, lanes);
    if (mUnit == 0) {
        for (s32 i = 0; i < 4; ++i) {
            if (Vu::writes(dest, i)) {
                mMemory.keepVuWindowWord(lanes[i]);
            }
        }
    }
}

void VuMicro::storeQuadword(u32 dest, u32 vf, u32 quadword) {
    if (!isAddressable(mUnit, quadword)) {
        return;
    }
    const u32 address = dataBase() + (quadword * 16u) % dataSize();
    const Reg128 value = mState.readVf(vf);
    for (s32 i = 0; i < 4; ++i) {
        if (Vu::writes(dest, i)) {
            mMemory.write32(address + 4u * static_cast<u32>(i), value.word(i));
        }
    }
}

u32 VuMicro::readInterfaceRegister(u32 offset) const {
    return mMemory.read32(kVifRegisters[mUnit] + offset);
}

// The packet names no length, so the transfer runs until the unit reports the
// tag that ended it.
void VuMicro::kickToGif(u32 quadword) {
    // Only the second unit has a path to the interface.
    if (mGif == nullptr) {
        return;
    }
    const u32 quadwords = dataSize() / 16u;
    mGif->beginPath(kPath1);
    for (u32 sent = 0; sent < quadwords; ++sent) {
        const u32 address = dataBase() + ((quadword + sent) % quadwords) * 16u;
        mGif->writeQuadword(mMemory.read64(address), mMemory.read64(address + 8));
        if (!mGif->isPacketOpen()) {
            break;
        }
    }
    mGif->endPath();
}

void VuMicro::branchTo(u32 target) {
    mBranchTarget = target;
    mBranching = true;
}

void VuMicro::executeLower(u32 code) {
    const u32 ft = ftOf(code), fs = fsOf(code), fd = fdOf(code);

    if (isRegisterForm(code)) {
        if ((code & 0x3Cu) == 0x3Cu) {
            const u32 wide = ((code >> 6) & 0x1Fu) << 6 | (code & 0x3Fu);
            const u32 dest = destOf(code);
            switch (wide) {
                case lower::kMove: vmove(mState, dest, ft, fs); return;
                case lower::kMr32: vmr32(mState, dest, ft, fs); return;
                case lower::kMfir: vmfir(mState, dest, ft, fs); return;
                case lower::kMtir:
                    vmtir(mState, ft, fs, static_cast<s32>((code >> 21) & 3u));
                    return;
                case lower::kRinit: vrinit(mState, sourceLane(code, fs)); return;
                case lower::kRxor: vrxor(mState, sourceLane(code, fs)); return;
                case lower::kRnext:
                    advanceRandom();
                    [[fallthrough]];
                case lower::kRget: vrget(mState, dest, ft); return;
                case lower::kMfp:
                    if (mUnit == 0) {
                        mState.vi[Vu::kP] = mMemory.getVuWindowWord();
                    }
                    vmfp(mState, dest, ft);
                    return;
                case lower::kWaitp: mFunction.settle(mState.vi[Vu::kP]); return;
                case lower::kXgkick: kickToGif(mState.readVi(fs)); return;
                case lower::kXtop: mState.writeVi(ft, readInterfaceRegister(kTop)); return;
                case lower::kXitop: mState.writeVi(ft, readInterfaceRegister(kItop)); return;
                case lower::kEsadd:
                case lower::kErsadd:
                case lower::kEleng:
                case lower::kErleng:
                case lower::kEatanxy:
                case lower::kEatanxz:
                case lower::kEsum:
                case lower::kEsqrt:
                case lower::kErsqrt:
                case lower::kErcpr:
                case lower::kEsin:
                case lower::kEatan:
                case lower::kEexp:
                    // The first unit has no elementary function unit, so the
                    // instruction that names one does nothing there.
                    if (mUnit == 1) {
                        mFunction.issue(mState.vi[Vu::kP],
                                        elementaryFunction(wide, code, fs),
                                        functionLatency(wide));
                    }
                    return;
                case lower::kDiv:
                    mQuotient.issue(mState.vi[Vu::kQ], quotientOf(code, fs, ft),
                                    kQuotientLatency);
                    return;
                case lower::kSqrt:
                    mQuotient.issue(mState.vi[Vu::kQ], rootOf(code, ft), kQuotientLatency);
                    return;
                case lower::kRsqrt:
                    mQuotient.issue(mState.vi[Vu::kQ], inverseRootOf(code, fs, ft),
                                    kRootLatency);
                    return;
                case lower::kWaitq: mQuotient.settle(mState.vi[Vu::kQ]); return;
                case lower::kIlwr:
                    vilwr(mState, mMemory, dest, ft, fs, dataBase(), dataSize());
                    return;
                case lower::kIswr:
                    viswr(mState, mMemory, dest, ft, fs, dataBase(), dataSize());
                    return;
                case lower::kLqi:
                    loadQuadword(dest, ft, mState.readVi(fs));
                    mState.writeVi(fs, mState.readVi(fs) + 1);
                    return;
                case lower::kSqi:
                    storeQuadword(dest, fs, mState.readVi(ft));
                    mState.writeVi(ft, mState.readVi(ft) + 1);
                    return;
                case lower::kLqd:
                    mState.writeVi(fs, mState.readVi(fs) - 1);
                    loadQuadword(dest, ft, mState.readVi(fs));
                    return;
                case lower::kSqd:
                    mState.writeVi(ft, mState.readVi(ft) - 1);
                    storeQuadword(dest, fs, mState.readVi(ft));
                    return;
                default: return;
            }
        }
        switch (code & 0x3Fu) {
            case lower::kIadd: viadd(mState, fd, fs, ft); return;
            case lower::kIsub: visub(mState, fd, fs, ft); return;
            case lower::kIand: viand(mState, fd, fs, ft); return;
            case lower::kIor: vior(mState, fd, fs, ft); return;
            case lower::kIaddi: viaddi(mState, ft, fs, signed5(code)); return;
            default: return;
        }
    }

    const u32 opcode = (code >> 25) & 0x7Fu;
    const s32 offset = signed11(code);
    const u32 next = mProgramCounter + 1;
    const u32 immediate24 = code & 0xFFFFFFu;
    switch (opcode) {
        case lower::kLq:
            loadQuadword(destOf(code), ft, mState.readVi(fs) + static_cast<u32>(offset));
            return;
        case lower::kSq:
            storeQuadword(destOf(code), fs, mState.readVi(ft) + static_cast<u32>(offset));
            return;
        case lower::kFcget: mState.writeVi(ft, mState.vi[Vu::kClip] & 0xFFFu); return;
        case lower::kFcset:
            mState.clip = immediate24;
            mState.vi[Vu::kClip] = immediate24;
            for (u32& pending : mClipDelayLine) {
                pending = immediate24;
            }
            return;
        case lower::kFcand:
            mState.writeVi(1, (mState.vi[Vu::kClip] & immediate24) != 0 ? 1u : 0u);
            return;
        case lower::kFcor:
            mState.writeVi(1, (mState.vi[Vu::kClip] | immediate24) == 0xFFFFFFu ? 1u : 0u);
            return;
        case lower::kFceq:
            mState.writeVi(1, mState.vi[Vu::kClip] == immediate24 ? 1u : 0u);
            return;
        case lower::kFsand:
            mState.writeVi(ft, mState.readSeenStatus() & statusImmediate(code));
            return;
        case lower::kFsor:
            mState.writeVi(ft, mState.readSeenStatus() | statusImmediate(code));
            return;
        case lower::kFsset:
            // The low bits are a view of the flag register, so a write reaches
            // only the sticky copies, the same as ctc2.
            mState.writeControl(Vu::kStatus, statusImmediate(code));
            return;
        case lower::kFseq:
            mState.writeVi(ft, mState.readSeenStatus() == statusImmediate(code) ? 1u : 0u);
            return;
        case lower::kFmand:
            mState.writeVi(ft, mState.readSeenMac() & mState.readVi(fs));
            return;
        case lower::kFmor:
            mState.writeVi(ft, mState.readSeenMac() | mState.readVi(fs));
            return;
        case lower::kFmeq:
            mState.writeVi(ft, mState.readSeenMac() == mState.readVi(fs) ? 1u : 0u);
            return;
        case lower::kIaddiu:
            mState.writeVi(ft, mState.readVi(fs) + immediate15(code));
            return;
        case lower::kIsubiu:
            mState.writeVi(ft, mState.readVi(fs) - immediate15(code));
            return;
        case lower::kIlw:
            vilwr(mState, mMemory, destOf(code), ft, fs, dataBase(), dataSize(), offset);
            return;
        case lower::kIsw:
            viswr(mState, mMemory, destOf(code), ft, fs, dataBase(), dataSize(), offset);
            return;
        case lower::kB: branchTo(next + static_cast<u32>(offset)); return;
        case lower::kBal:
            mState.writeVi(ft, next + 1);
            branchTo(next + static_cast<u32>(offset));
            return;
        case lower::kJr: branchTo(mState.readVi(fs)); return;
        case lower::kJalr: {
            const u32 target = mState.readVi(fs);
            mState.writeVi(ft, next + 1);
            branchTo(target);
            return;
        }
        case lower::kIbeq:
            if (mState.readVi(ft) == mState.readVi(fs)) {
                branchTo(next + static_cast<u32>(offset));
            }
            return;
        case lower::kIbne:
            if (mState.readVi(ft) != mState.readVi(fs)) {
                branchTo(next + static_cast<u32>(offset));
            }
            return;
        case lower::kIbltz:
            if (static_cast<s16>(mState.readVi(fs)) < 0) {
                branchTo(next + static_cast<u32>(offset));
            }
            return;
        case lower::kIbgtz:
            if (static_cast<s16>(mState.readVi(fs)) > 0) {
                branchTo(next + static_cast<u32>(offset));
            }
            return;
        case lower::kIblez:
            if (static_cast<s16>(mState.readVi(fs)) <= 0) {
                branchTo(next + static_cast<u32>(offset));
            }
            return;
        case lower::kIbgez:
            if (static_cast<s16>(mState.readVi(fs)) >= 0) {
                branchTo(next + static_cast<u32>(offset));
            }
            return;
        default: return;
    }
}

// The upper result lands several cycles after the pair issues, so the lower
// instruction beside it reads the register as it was. Where both write the
// same register, the lanes the upper one names are the ones that stick.
void VuMicro::executeLowerBeside(u32 upper, u32 lower, const Reg128& upperBefore) {
    const u32 target = upperDestination(upper);
    if (target == 0) {
        executeLower(lower);
        return;
    }
    const Reg128 upperResult = mState.vf[target];
    mState.vf[target] = upperBefore;
    executeLower(lower);
    const u32 dest = destOf(upper);
    for (s32 i = 0; i < 4; ++i) {
        if (Vu::writes(dest, i)) {
            mState.vf[target].setWord(i, upperResult.word(i));
        }
    }
}

// The end bit stops the program one instruction later, so the pair that carries
// it and the one behind it still run.
VuMicro::Outcome VuMicro::run(u32 start) {
    begin(start);
    for (u32 steps = 0; steps < kRunawayStep; ++steps) {
        if (step()) {
            // The counter names where the program would have gone next, and
            // wraps within the instruction memory like a fetch does.
            return {(mProgramCounter + 1) % instructionCount(), true};
        }
    }
    settlePipelines();
    return {mProgramCounter, false};
}

void VuMicro::begin(u32 start) {
    mProgramCounter = start;
    mBranching = false;
    mEnding = false;
    mEndDelay = 1;
    for (u32& pending : mClipDelayLine) {
        pending = mState.vi[Vu::kClip];
    }
    mQuotient = {};
    mFunction = {};
    mCycle = 0;
    for (u32& ready : mWriteReady) {
        ready = 0;
    }
}

bool VuMicro::step() {
    const u32 address = instructionBase() + (mProgramCounter % instructionCount()) * 8;
    const u32 lower = mMemory.read32(address);
    const u32 upper = mMemory.read32(address + 4);

    stallUntil(operandsReadyAt(upper, lower));

    const u32 priorStatus = mState.vi[Vu::kStatus];
    const u32 priorMac = mState.vi[Vu::kMac];

    const bool taken = mBranching;
    const u32 target = mBranchTarget;
    mBranching = false;

    mState.vi[Vu::kClip] = mClipDelayLine[0];
    for (u32 i = 0; i + 1 < kClipDelay; ++i) {
        mClipDelayLine[i] = mClipDelayLine[i + 1];
    }

    // A wait is a stall, so the result it waits for is already there for the
    // vector op paired with it.
    if (isRegisterForm(lower)) {
        const u32 wide = ((lower >> 6) & 0x1Fu) << 6 | (lower & 0x3Fu);
        if (wide == lower::kWaitq) {
            mQuotient.settle(mState.vi[Vu::kQ]);
        } else if (wide == lower::kWaitp) {
            mFunction.settle(mState.vi[Vu::kP]);
        }
    }

    if ((upper & kEndBit) != 0) {
        mEnding = true;
    }
    const Reg128 upperBefore = mState.vf[upperDestination(upper)];
    executeUpper(upper);
    if (upperDestination(upper) != 0) {
        mWriteReady[upperDestination(upper)] = mCycle + kFmacLatency;
    }
    if ((upper & kImmediateBit) != 0) {
        mState.vi[Vu::kI] = lower;
    } else {
        executeLowerBeside(upper, lower, upperBefore);
    }

    mClipDelayLine[kClipDelay - 1] = mState.clip;
    advancePipelines();
    mState.advanceFlags();
    if (mState.vi[Vu::kStatus] != priorStatus || mState.vi[Vu::kMac] != priorMac) {
        mState.stageFlags(priorStatus & Vu::kStickyMask);
    }
    ++mCycle;

    if (mEnding && mEndDelay-- == 0) {
        // The pipeline has drained by the time the program stops, so the
        // register a guest reads afterwards holds the final result.
        mState.vi[Vu::kClip] = mState.clip;
        settlePipelines();
        return true;
    }
    mProgramCounter = taken ? target : mProgramCounter + 1;
    return false;
}

bool runMicroProgram(Vu& state, Memory& memory, Gif* gif, u32 unit, u32 start) {
    const VuMicro::Outcome outcome = VuMicro(state, memory, unit, gif).run(start);
    state.vi[Vu::kTpc] = outcome.counter;
    return outcome.ended;
}

}  // namespace uee
