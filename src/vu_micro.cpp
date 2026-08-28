#include "vu_micro.h"

#include <algorithm>
#include <cmath>

#include "memory.h"
#include "vu_handlers.h"

namespace uee {
namespace {
constexpr u32 kOne = 0x3F800000u;
constexpr u32 kSine[5] = {0x3F800000u, 0xBE2AAAA5u, 0x3C08873Fu, 0xB94FB21Fu, 0x362E9C14u};
constexpr u32 kArcTangent[8] = {0x3F7FFFF2u, 0xBEAAA61Cu, 0x3E4C40A6u, 0xBE0E6C63u,
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

inline u32 divide(u32 a, u32 b) {
    if (single::widen(b) == 0.0) {
        return ((a ^ b) & single::kSignMask) | single::kLargest;
    }
    return single::div(a, b).bits;
}

inline u32 root(u32 a) {
    return single::sqrt(a).bits;
}

inline u32 inverseRoot(u32 a, u32 b) {
    return divide(a, root(b));
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
    accumulator = accumulate(accumulator, kOne, kQuarterTurn);
    return accumulate(accumulator, odd, kArcTangent[7]);
}

inline u32 arcTangent(u32 numerator, u32 denominator) {
    const u32 top = single::sub(numerator, denominator).bits;
    const u32 bottom = single::add(numerator, denominator).bits;
    if ((bottom & single::kMagnitudeMask) == 0) {
        return ((top ^ bottom) & single::kSignMask) | kOverflowedRightAngle;
    }
    return arcTangentOfRatio(divide(top, bottom));
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
    return divide(kOne, value);
}


constexpr u32 kEndBit = 1u << 30;
constexpr u32 kImmediateBit = 1u << 31;

constexpr bool isRegisterForm(u32 code) {
    return (code & 0x80000000u) != 0;
}

constexpr u32 destOf(u32 code) { return (code >> 21) & 0xFu; }
constexpr u32 ftOf(u32 code) { return (code >> 16) & 0x1Fu; }
constexpr u32 fsOf(u32 code) { return (code >> 11) & 0x1Fu; }
constexpr u32 fdOf(u32 code) { return (code >> 6) & 0x1Fu; }
constexpr u32 broadcastOf(u32 code) { return code & 3u; }

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
}  // namespace lower

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
        case 0x13C: vitof0(mState, dest, ft, fs); return;
        case 0x17C: vftoi0(mState, dest, ft, fs); return;
        case 0x1FD: vabs(mState, dest, ft, fs); return;
        case 0x1FF: vclip(mState, fs, ft); return;
        case 0x2FC: vopmula(mState, dest, fs, ft); return;
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

void VuMicro::stallUntil(u32 cycle) {
    while (mCycle < cycle) {
        advanceQuotients();
        ++mCycle;
    }
}

// A divide issued while the unit is still busy stalls until it frees, so the
// result already in flight lands before this one even starts counting.
void VuMicro::issueQuotient(u32 value, u32 latency) {
    settleQuotients();
    mPendingQuotient = value;
    mQuotientDelay = latency;
}

void VuMicro::advanceQuotients() {
    if (mQuotientDelay != 0 && --mQuotientDelay == 0) {
        mState.vi[Vu::kQ] = mPendingQuotient;
    }
}

void VuMicro::settleQuotients() {
    if (mQuotientDelay != 0) {
        mState.vi[Vu::kQ] = mPendingQuotient;
        mQuotientDelay = 0;
    }
}

u32 VuMicro::elementaryFunction(u32 wide, u32 code, u32 vs) {
    const Reg128 source = mState.readVf(vs);
    const u32 x = source.word(0), y = source.word(1);
    const u32 z = source.word(2), w = source.word(3);
    const u32 scalar = sourceLane(code, vs);
    const u32 square = sumOfSquares(x, y, z);

    switch (wide) {
        case lower::kEsadd: return square;
        case lower::kErsadd: return divide(kOne, square);
        case lower::kEleng: return root(square);
        case lower::kErleng: return inverseRoot(kOne, square);
        case lower::kEatanxy: return arcTangent(y, x);
        case lower::kEatanxz: return arcTangent(z, x);
        case lower::kEsum: return sum(x, y, z, w);
        case lower::kEsqrt: return root(scalar);
        case lower::kErsqrt: return inverseRoot(kOne, scalar);
        case lower::kErcpr: return divide(kOne, scalar);
        case lower::kEsin: return sine(scalar);
        case lower::kEatan: return arcTangent(scalar, kOne);
        case lower::kEexp: return exponential(scalar);
        default: return 0;
    }
}

u32 VuMicro::quotientOf(u32 code, u32 vs, u32 vt) {
    Vu scratch = mState;
    vdiv(scratch, vs, (code >> 21) & 3u, vt, (code >> 23) & 3u);
    return scratch.vi[Vu::kQ];
}

u32 VuMicro::rootOf(u32 code, u32 vt) {
    Vu scratch = mState;
    vsqrt(scratch, vt, (code >> 23) & 3u);
    return scratch.vi[Vu::kQ];
}

u32 VuMicro::inverseRootOf(u32 code, u32 vs, u32 vt) {
    Vu scratch = mState;
    vrsqrt(scratch, vs, (code >> 21) & 3u, vt, (code >> 23) & 3u);
    return scratch.vi[Vu::kQ];
}

u32 VuMicro::sourceLane(u32 code, u32 vf) const {
    return mState.readVf(vf).word(static_cast<s32>((code >> 21) & 3u));
}

void VuMicro::advanceRandom() {
    u32& value = mState.vi[Vu::kR];
    const u32 bit = ((value >> 4) ^ (value >> 22)) & 1u;
    value = ((value << 1) | bit) & 0x7FFFFFu;
}

void VuMicro::loadQuadword(u32 dest, u32 vf, u32 quadword) {
    const u32 address = dataBase() + (quadword * 16u) % dataSize();
    u32 lanes[4];
    for (s32 i = 0; i < 4; ++i) {
        lanes[i] = mMemory.read32(address + 4u * static_cast<u32>(i));
    }
    mState.writeVf(vf, dest, lanes);
}

void VuMicro::storeQuadword(u32 dest, u32 vf, u32 quadword) {
    const u32 address = dataBase() + (quadword * 16u) % dataSize();
    const Reg128 value = mState.readVf(vf);
    for (s32 i = 0; i < 4; ++i) {
        if (Vu::writes(dest, i)) {
            mMemory.write32(address + 4u * static_cast<u32>(i), value.word(i));
        }
    }
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
                case lower::kMfp: vmfp(mState, dest, ft); return;
                case lower::kWaitp:
                    return;
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
                    mState.vi[Vu::kP] = elementaryFunction(wide, code, fs);
                    return;
                case lower::kDiv:
                    issueQuotient(quotientOf(code, fs, ft), kQuotientLatency);
                    return;
                case lower::kSqrt:
                    issueQuotient(rootOf(code, ft), kQuotientLatency);
                    return;
                case lower::kRsqrt:
                    issueQuotient(inverseRootOf(code, fs, ft), kRootLatency);
                    return;
                case lower::kWaitq: settleQuotients(); return;
                case lower::kIlwr: vilwr(mState, mMemory, dest, ft, fs); return;
                case lower::kIswr: viswr(mState, mMemory, dest, ft, fs); return;
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
            mState.writeVi(ft, mState.vi[Vu::kStatus] & (code & 0xFFFu));
            return;
        case lower::kFsor:
            mState.writeVi(ft, mState.vi[Vu::kStatus] | (code & 0xFFFu));
            return;
        case lower::kFsset:
            mState.vi[Vu::kStatus] = code & 0xFFFu;
            return;
        case lower::kFseq:
            mState.writeVi(ft, mState.vi[Vu::kStatus] == (code & 0xFFFu) ? 1u : 0u);
            return;
        case lower::kFmand:
            mState.writeVi(ft, mState.vi[Vu::kMac] & mState.readVi(fs));
            return;
        case lower::kFmor:
            mState.writeVi(ft, mState.vi[Vu::kMac] | mState.readVi(fs));
            return;
        case lower::kFmeq:
            mState.writeVi(ft, mState.vi[Vu::kMac] == mState.readVi(fs) ? 1u : 0u);
            return;
        case lower::kIaddiu:
            mState.writeVi(ft, mState.readVi(fs) + immediate15(code));
            return;
        case lower::kIsubiu:
            mState.writeVi(ft, mState.readVi(fs) - immediate15(code));
            return;
        case lower::kIlw:
            mState.writeVi(ft, mMemory.read32(dataBase() +
                                              ((mState.readVi(fs) * 16u + (code & 0x7FFu) * 16u) %
                                               dataSize())));
            return;
        case lower::kIsw:
            mMemory.write32(dataBase() + ((mState.readVi(fs) * 16u + (code & 0x7FFu) * 16u) %
                                          dataSize()),
                            mState.readVi(ft));
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

// The end bit stops the program two instructions later, so the pair that
// carries it and the pair behind it still run.
u32 VuMicro::run(u32 start) {
    mProgramCounter = start;
    mBranching = false;
    mEnding = false;
    for (u32& pending : mClipDelayLine) {
        pending = mState.clip;
    }
    mQuotientDelay = 0;
    mCycle = 0;
    for (u32& ready : mWriteReady) {
        ready = 0;
    }

    u32 remaining = 2;
    for (u32 steps = 0; steps < instructionCount() * 4; ++steps) {
        const u32 address = instructionBase() + (mProgramCounter % instructionCount()) * 8;
        const u32 lower = mMemory.read32(address);
        const u32 upper = mMemory.read32(address + 4);

        stallUntil(operandsReadyAt(upper, lower));

        const bool taken = mBranching;
        const u32 target = mBranchTarget;
        mBranching = false;

        mState.vi[Vu::kClip] = mClipDelayLine[0];
        for (u32 i = 0; i + 1 < kClipDelay; ++i) {
            mClipDelayLine[i] = mClipDelayLine[i + 1];
        }

        // A wait is a stall, so the quotient it waits for is already there for
        // the vector op paired with it.
        if (isRegisterForm(lower) &&
            (((lower >> 6) & 0x1Fu) << 6 | (lower & 0x3Fu)) == lower::kWaitq) {
            settleQuotients();
        }

        if ((upper & kEndBit) != 0) {
            mEnding = true;
        }
        executeUpper(upper);
        if (fdOf(upper) != 0) {
            mWriteReady[fdOf(upper)] = mCycle + kFmacLatency;
        }
        if ((upper & kImmediateBit) != 0) {
            mState.vi[Vu::kI] = lower;
        } else {
            executeLower(lower);
        }

        mClipDelayLine[kClipDelay - 1] = mState.clip;
        advanceQuotients();
        ++mCycle;

        if (mEnding && remaining-- == 0) {
            // The pipeline has drained by the time the program stops, so the
            // register a guest reads afterwards holds the final result.
            mState.vi[Vu::kClip] = mState.clip;
            return mProgramCounter;
        }
        mProgramCounter = taken ? target : mProgramCounter + 1;
    }
    return mProgramCounter;
}

}  // namespace uee
