#include "vu_micro.h"

#include <algorithm>
#include <cmath>
#include <cstring>
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

constexpr u32 kEndBit = 1u << 30;
constexpr u32 kImmediateBit = 1u << 31;

constexpr bool isRegisterForm(u32 code) {
    return (code & 0x80000000u) != 0;
}

constexpr u32 getDest(u32 code) { return (code >> 21) & 0xFu; }
constexpr u32 getFt(u32 code) { return (code >> 16) & 0x1Fu; }
constexpr u32 getFs(u32 code) { return (code >> 11) & 0x1Fu; }
constexpr u32 getFd(u32 code) { return (code >> 6) & 0x1Fu; }
constexpr u32 getBroadcast(u32 code) { return code & 3u; }
// Opcodes ending in 0x3C-0x3F take five more bits from where fd would be.
constexpr u32 getWideOpcode(u32 code) { return ((code >> 6) & 0x1Fu) << 6 | (code & 0x3Fu); }
constexpr bool isWide(u32 code) { return (code & 0x3Cu) == 0x3Cu; }

constexpr u32 kXyz = 0xEu;
constexpr u32 kW = 0x1u;
constexpr u32 getLaneMask(u32 field) { return 8u >> field; }

// The status ops carry a twelve bit immediate with its top bit ten places up
// from the rest.
constexpr u32 getStatusImmediate(u32 code) {
    return (code & 0x7FFu) | ((code >> 10) & 0x800u);
}

// The vector register an upper instruction writes, or zero for one that writes
// only the accumulator, the clip flags or nothing.
constexpr u32 getUpperDestination(u32 code) {
    if ((code & 0x3Fu) < 0x30u) {
        return getFd(code);
    }
    if (!isWide(code)) {
        return 0;
    }
    switch (getWideOpcode(code)) {
        case 0x13C: case 0x13D: case 0x13E: case 0x13F:
        case 0x17C: case 0x17D: case 0x17E: case 0x17F:
        case 0x1FD:
            return getFt(code);
        default:
            return 0;
    }
}

constexpr u32 getImmediate15(u32 code) {
    return (code & 0x7FFu) | ((code >> 10) & 0x7800u);
}

constexpr s32 getSigned11(u32 code) {
    return static_cast<s32>((code & 0x7FFu) << 21) >> 21;
}

constexpr s32 getSigned5(u32 code) {
    return static_cast<s32>(((code >> 6) & 0x1Fu) << 27) >> 27;
}

constexpr VuSource getBroadcastSource(u32 code) {
    switch (getBroadcast(code)) {
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

// VU0's 4 KB repeat over the 16 KB it decodes and nothing lies past them; VU1
// fills its decode, so every address wraps.
bool isAddressable(u32 unit, u32 quadword) {
    constexpr u32 kDecodedQuadwords = 0x400;
    if (unit != 0) {
        return true;
    }
    return static_cast<s16>(quadword) >= 0 && (quadword & 0xFFFFu) < kDecodedQuadwords;
}

// The hardware unit runs beside the processor, so a program with no end bit
// spins there forever while the guest carries on. Running it here has to stop
// somewhere, and a program that means to end does so long before this.
constexpr u32 kRunawayStep = 1'000'000;

constexpr u32 kVifRegisters[2] = {0x10003800, 0x10003C00};
constexpr u32 kItop = 0xD0, kTop = 0xE0;

std::span<u8> getUnitMemory(Memory& memory, u32 base, u32 size) {
    const std::span<u8> bytes = memory.regionAt(base);
    if (bytes.size() != size) {
        throw std::logic_error(std::format("expected {} bytes of unit memory at {:08x}, found {}",
                                           size, base, bytes.size()));
    }
    return bytes;
}

u32 readWord(std::span<const u8> bytes, u32 offset) {
    u32 value;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

u64 readDouble(std::span<const u8> bytes, u32 offset) {
    u64 value;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

void writeWord(std::span<u8> bytes, u32 offset, u32 value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

}  // namespace

VuMicro::VuMicro(Vu& state, Memory& memory, u32 unit, Gif* gif)
    : mState(state), mMemory(memory), mGif(gif), mUnit(unit),
      mInstructions(getUnitMemory(memory, unit == 0 ? kVu0MicroBase : kVu1MicroBase,
                                  unit == 0 ? kVu0MemorySize : kVu1MemorySize)),
      mData(getUnitMemory(memory, getDataBase(), getDataSize())) {}

u32 VuMicro::getInstructionCount() const { return static_cast<u32>(mInstructions.size() / 8); }
u32 VuMicro::getDataBase() const { return mUnit == 0 ? kVu0DataBase : kVu1DataBase; }
u32 VuMicro::getDataSize() const { return mUnit == 0 ? kVu0MemorySize : kVu1MemorySize; }

void VuMicro::executeUpper(u32 code) {
    const u32 dest = getDest(code);
    const u32 fd = getFd(code), fs = getFs(code), ft = getFt(code);
    const u32 opcode = code & 0x3Fu;

    switch (opcode >> 2) {
        case 0x0: vadd(mState, getBroadcastSource(code), dest, fd, fs, ft); return;
        case 0x1: vsub(mState, getBroadcastSource(code), dest, fd, fs, ft); return;
        case 0x2: vmadd(mState, getBroadcastSource(code), dest, fd, fs, ft); return;
        case 0x3: vmsub(mState, getBroadcastSource(code), dest, fd, fs, ft); return;
        case 0x4: vmax(mState, getBroadcastSource(code), dest, fd, fs, ft); return;
        case 0x5: vmini(mState, getBroadcastSource(code), dest, fd, fs, ft); return;
        case 0x6: vmul(mState, getBroadcastSource(code), dest, fd, fs, ft); return;
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

    switch (getWideOpcode(code)) {
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

// Hazards are checked a field at a time, so writing one lane of a register
// holds up only the instructions that read that lane.
u32 VuMicro::getLaneReadyCycle(u32 vf, u32 lanes) const {
    u32 ready = 0;
    for (s32 i = 0; vf != 0 && i < 4; ++i) {
        if (Vu::writes(lanes, i)) {
            ready = std::max(ready, mLaneReady[vf][i]);
        }
    }
    return ready;
}

u32 VuMicro::getUpperReadyCycle(u32 code) const {
    const u32 dest = getDest(code);
    auto reads = [&](u32 fsLanes, u32 ftLanes) {
        return std::max(getLaneReadyCycle(getFs(code), fsLanes), getLaneReadyCycle(getFt(code), ftLanes));
    };
    const u32 opcode = code & 0x3Fu;
    if (opcode < 0x1C) {
        return reads(dest, getLaneMask(getBroadcast(code)));
    }
    if (opcode < 0x28) {
        return reads(dest, 0);  // the Q and I forms
    }
    if (opcode == 0x2E) {
        return reads(kXyz, kXyz);  // OPMSUB
    }
    if (opcode < 0x30) {
        return reads(dest, dest);
    }
    if (!isWide(code)) {
        return 0;
    }
    switch (getWideOpcode(code)) {
        case 0x1FF: return reads(kXyz, kW);    // CLIP
        case 0x2FE: return reads(kXyz, kXyz);  // OPMULA
        case 0x2BF:
        case 0x2FF: return 0;
        default: break;
    }
    switch (getWideOpcode(code) >> 6) {
        case 0: case 1: case 2: case 3: case 6:
            return reads(dest, getLaneMask(getBroadcast(code)));
        case 10: case 11:
            return reads(dest, dest);
        default:
            return reads(dest, 0);
    }
}

u32 VuMicro::getLowerReadyCycle(u32 code) const {
    const u32 dest = getDest(code), fs = getFs(code), ft = getFt(code);
    const u32 fsField = getLaneMask((code >> 21) & 3u);
    const u32 ftField = getLaneMask((code >> 23) & 3u);
    if (!isRegisterForm(code)) {
        switch ((code >> 25) & 0x7Fu) {
            case lower::kSq: return getLaneReadyCycle(fs, dest);
            case lower::kIbeq:
            case lower::kIbne:
                return std::max(mIntegerReady[fs & 0xFu], mIntegerReady[ft & 0xFu]);
            case lower::kIbltz:
            case lower::kIbgtz:
            case lower::kIblez:
            case lower::kIbgez:
            case lower::kJr:
            case lower::kJalr:
                return mIntegerReady[fs & 0xFu];
            default: return 0;
        }
    }
    if (!isWide(code)) {
        return 0;
    }
    // The divider and the function unit take one op at a time, so another op
    // or a wait holds until the one in flight lands.
    const u32 dividerFree = mCycle + mQuotient.delay;
    const u32 functionFree = mCycle + mFunction.delay;
    switch (getWideOpcode(code)) {
        case lower::kMove:
        case lower::kSqi:
        case lower::kSqd:
            return getLaneReadyCycle(fs, dest);
        case lower::kMr32: return getLaneReadyCycle(fs, (dest >> 1) | ((dest << 3) & 8u));
        case lower::kMtir:
        case lower::kRinit:
        case lower::kRxor:
            return getLaneReadyCycle(fs, fsField);
        case lower::kDiv:
        case lower::kRsqrt:
            return std::max({getLaneReadyCycle(fs, fsField), getLaneReadyCycle(ft, ftField), dividerFree});
        case lower::kSqrt: return std::max(getLaneReadyCycle(ft, ftField), dividerFree);
        case lower::kWaitq: return dividerFree;
        case lower::kEsadd:
        case lower::kErsadd:
        case lower::kEleng:
        case lower::kErleng:
            return std::max(getLaneReadyCycle(fs, kXyz), functionFree);
        case lower::kEatanxy: return std::max(getLaneReadyCycle(fs, 0xCu), functionFree);
        case lower::kEatanxz: return std::max(getLaneReadyCycle(fs, 0xAu), functionFree);
        case lower::kEsum: return std::max(getLaneReadyCycle(fs, 0xFu), functionFree);
        case lower::kEsqrt:
        case lower::kErsqrt:
        case lower::kErcpr:
        case lower::kEsin:
        case lower::kEatan:
        case lower::kEexp:
            return std::max(getLaneReadyCycle(fs, fsField), functionFree);
        case lower::kWaitp: return functionFree;
        default: return 0;
    }
}

u32 VuMicro::getOperandsReadyCycle(u32 upper, u32 lower) const {
    const u32 ready = getUpperReadyCycle(upper);
    if ((upper & kImmediateBit) != 0) {
        return ready;
    }
    return std::max(ready, getLowerReadyCycle(lower));
}

// A result reaches its register through the same four stages whichever half
// of the pair produced it.
void VuMicro::markWritten(u32 upper, u32 lower) {
    auto mark = [&](u32 vf, u32 lanes) {
        for (s32 i = 0; i < 4; ++i) {
            if (vf != 0 && Vu::writes(lanes, i)) {
                mLaneReady[vf][i] = mCycle + kFmacLatency;
            }
        }
    };
    mark(getUpperDestination(upper), getDest(upper));
    if ((upper & kImmediateBit) != 0) {
        return;
    }
    if (!isRegisterForm(lower)) {
        const u32 opcode = (lower >> 25) & 0x7Fu;
        if (opcode == lower::kLq) {
            mark(getFt(lower), getDest(lower));
        } else if (opcode == lower::kIlw) {
            mIntegerReady[getFt(lower) & 0xFu] = mCycle + kIntegerLoadLatency;
        }
        return;
    }
    if (!isWide(lower)) {
        return;
    }
    switch (getWideOpcode(lower)) {
        case lower::kLqi:
        case lower::kLqd:
        case lower::kMove:
        case lower::kMr32:
        case lower::kMfir:
        case lower::kRget:
        case lower::kRnext:
        case lower::kMfp:
            mark(getFt(lower), getDest(lower));
            return;
        case lower::kIlwr:
            mIntegerReady[getFt(lower) & 0xFu] = mCycle + kIntegerLoadLatency;
            return;
        default: return;
    }
}

// The op that set a flag keeps moving while the one waiting on it stalls, so
// the flags land after the same number of cycles either way.
void VuMicro::stallUntil(u32 cycle) {
    while (mCycle < cycle) {
        beginCycle();
        endCycle();
    }
}

void VuMicro::beginCycle() {
    mReadableFlags = mFlagDelayLine[0];
    mState.vi[Vu::kClip] = mReadableFlags.clip;
    std::copy(std::begin(mFlagDelayLine) + 1, std::end(mFlagDelayLine), std::begin(mFlagDelayLine));
}

void VuMicro::endCycle() {
    mFlagDelayLine[kFlagDelay - 1] = {mState.vi[Vu::kStatus], mState.vi[Vu::kMac], mState.clip};
    mQuotient.advance(mState.vi[Vu::kQ]);
    mFunction.advance(mState.vi[Vu::kP]);
    ++mCycle;
}

// The unit keeps working after the program stops, so the result lands whether
// or not anything is left to read it.
void VuMicro::syncPipelines() {
    mState.syncFlags();
    mState.vi[Vu::kClip] = mState.clip;
    mQuotient.sync(mState.vi[Vu::kQ]);
    mFunction.sync(mState.vi[Vu::kP]);
}

u32 VuMicro::computeFunction(u32 wide, u32 code, u32 vs) {
    const Reg128 source = mState.readVf(vs);
    const u32 x = source.word(0), y = source.word(1);
    const u32 z = source.word(2), w = source.word(3);
    const u32 scalar = readFsLane(code, vs);
    const u32 square = sumSquares(x, y, z);

    switch (wide) {
        case lower::kEsadd: return square;
        case lower::kErsadd: return computeEfuQuotient(kOne, square);
        case lower::kEleng: return computeEfuRoot(square);
        case lower::kErleng: return computeEfuInverseRoot(kOne, square);
        case lower::kEatanxy: return computeArcTangent(y, x);
        case lower::kEatanxz: return computeArcTangent(z, x);
        case lower::kEsum: return sumLanes(x, y, z, w);
        case lower::kEsqrt: return computeEfuRoot(scalar);
        case lower::kErsqrt: return computeEfuInverseRoot(kOne, scalar);
        case lower::kErcpr: return computeEfuQuotient(kOne, scalar);
        case lower::kEsin: return computeSine(scalar);
        case lower::kEatan: return computeArcTangent(scalar, kOne);
        case lower::kEexp: return computeExponential(scalar);
        default: return 0;
    }
}

// How long each function takes to reach the result register, which a program
// has to leave between the op and the read.
u32 VuMicro::getFunctionLatency(u32 wide) {
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

u32 VuMicro::computeQuotient(u32 code, u32 vs, u32 vt) {
    return takeQuotient(computeVuQuotient(readFsLane(code, vs), readFtLane(code, vt)));
}

u32 VuMicro::computeRoot(u32 code, u32 vt) {
    return takeQuotient(computeVuRoot(readFtLane(code, vt)));
}

u32 VuMicro::computeInverseRoot(u32 code, u32 vs, u32 vt) {
    return takeQuotient(computeVuInverseRoot(readFsLane(code, vs), readFtLane(code, vt)));
}

u32 VuMicro::takeQuotient(Vu::Quotient q) {
    mState.setDivideFlags(q.invalid, q.byZero);
    return q.value;
}

u32 VuMicro::readFtLane(u32 code, u32 vf) const {
    return mState.readVf(vf).word(static_cast<s32>((code >> 23) & 3u));
}

u32 VuMicro::readFsLane(u32 code, u32 vf) const {
    return mState.readVf(vf).word(static_cast<s32>((code >> 21) & 3u));
}

void VuMicro::advanceRandom() {
    u32& value = mState.vi[Vu::kR];
    const u32 bit = ((value >> 4) ^ (value >> 22)) & 1u;
    value = ((value << 1) | bit) & 0x7FFFFFu;
}

void VuMicro::loadQuadword(u32 dest, u32 vf, u32 quadword) {
    if (!isAddressable(mUnit, quadword)) {
        const u32 zero[4]{};
        mState.writeVf(vf, dest, zero);
        return;
    }
    const u32 offset = (quadword * 16u) % getDataSize();
    u32 lanes[4];
    for (s32 i = 0; i < 4; ++i) {
        lanes[i] = readWord(mData, offset + 4u * static_cast<u32>(i));
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
    const u32 offset = (quadword * 16u) % getDataSize();
    const Reg128 value = mState.readVf(vf);
    for (s32 i = 0; i < 4; ++i) {
        if (Vu::writes(dest, i)) {
            writeWord(mData, offset + 4u * static_cast<u32>(i), value.word(i));
            mMemory.keepVuWindowWord(value.word(i));
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
    const u32 quadwords = getDataSize() / 16u;
    mGif->beginPath(kPath1);
    for (u32 sent = 0; sent < quadwords; ++sent) {
        const u32 offset = ((quadword + sent) % quadwords) * 16u;
        mGif->writeQuadword(readDouble(mData, offset), readDouble(mData, offset + 8));
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
    const u32 ft = getFt(code), fs = getFs(code), fd = getFd(code);

    if (isRegisterForm(code)) {
        if (isWide(code)) {
            const u32 wide = getWideOpcode(code);
            const u32 dest = getDest(code);
            switch (wide) {
                case lower::kMove: vmove(mState, dest, ft, fs); return;
                case lower::kMr32: vmr32(mState, dest, ft, fs); return;
                case lower::kMfir: vmfir(mState, dest, ft, fs); return;
                case lower::kMtir:
                    vmtir(mState, ft, fs, static_cast<s32>((code >> 21) & 3u));
                    return;
                case lower::kRinit: vrinit(mState, readFsLane(code, fs)); return;
                case lower::kRxor: vrxor(mState, readFsLane(code, fs)); return;
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
                        mFunction.issue(computeFunction(wide, code, fs), getFunctionLatency(wide));
                    }
                    return;
                case lower::kDiv: mQuotient.issue(computeQuotient(code, fs, ft), kQuotientLatency); return;
                case lower::kSqrt: mQuotient.issue(computeRoot(code, ft), kQuotientLatency); return;
                case lower::kRsqrt:
                    mQuotient.issue(computeInverseRoot(code, fs, ft), kRootLatency);
                    return;
                case lower::kIlwr:
                    vilwr(mState, mMemory, dest, ft, fs, getDataBase(), getDataSize());
                    return;
                case lower::kIswr:
                    viswr(mState, mMemory, dest, ft, fs, getDataBase(), getDataSize());
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
            case lower::kIaddi: viaddi(mState, ft, fs, getSigned5(code)); return;
            default: return;
        }
    }

    const u32 opcode = (code >> 25) & 0x7Fu;
    const s32 offset = getSigned11(code);
    const u32 next = mProgramCounter + 1;
    const u32 immediate24 = code & 0xFFFFFFu;
    switch (opcode) {
        case lower::kLq:
            loadQuadword(getDest(code), ft, mState.readVi(fs) + static_cast<u32>(offset));
            return;
        case lower::kSq:
            storeQuadword(getDest(code), fs, mState.readVi(ft) + static_cast<u32>(offset));
            return;
        case lower::kFcget: mState.writeVi(ft, mReadableFlags.clip & 0xFFFu); return;
        case lower::kFcset:
            mState.clip = immediate24;
            mState.vi[Vu::kClip] = immediate24;
            mReadableFlags.clip = immediate24;
            for (FlagSet& pending : mFlagDelayLine) {
                pending.clip = immediate24;
            }
            return;
        case lower::kFcand:
            mState.writeVi(1, (mReadableFlags.clip & immediate24) != 0 ? 1u : 0u);
            return;
        case lower::kFcor:
            mState.writeVi(1, (mReadableFlags.clip | immediate24) == 0xFFFFFFu ? 1u : 0u);
            return;
        case lower::kFceq:
            mState.writeVi(1, mReadableFlags.clip == immediate24 ? 1u : 0u);
            return;
        case lower::kFsand:
            mState.writeVi(ft, mReadableFlags.status & getStatusImmediate(code));
            return;
        case lower::kFsor:
            mState.writeVi(ft, mReadableFlags.status | getStatusImmediate(code));
            return;
        case lower::kFsset:
            // The low bits are a view of the flag register, so a write reaches
            // only the sticky copies, the same as ctc2.
            mState.writeControl(Vu::kStatus, getStatusImmediate(code));
            return;
        case lower::kFseq:
            mState.writeVi(ft, mReadableFlags.status == getStatusImmediate(code) ? 1u : 0u);
            return;
        case lower::kFmand:
            mState.writeVi(ft, mReadableFlags.mac & mState.readVi(fs));
            return;
        case lower::kFmor:
            mState.writeVi(ft, mReadableFlags.mac | mState.readVi(fs));
            return;
        case lower::kFmeq:
            mState.writeVi(ft, mReadableFlags.mac == mState.readVi(fs) ? 1u : 0u);
            return;
        case lower::kIaddiu:
            mState.writeVi(ft, mState.readVi(fs) + getImmediate15(code));
            return;
        case lower::kIsubiu:
            mState.writeVi(ft, mState.readVi(fs) - getImmediate15(code));
            return;
        case lower::kIlw:
            vilwr(mState, mMemory, getDest(code), ft, fs, getDataBase(), getDataSize(), offset);
            return;
        case lower::kIsw:
            viswr(mState, mMemory, getDest(code), ft, fs, getDataBase(), getDataSize(), offset);
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

// The lower op reads what the upper one writes as it was before the pair; where
// both write a register, the upper op's lanes win.
void VuMicro::executeLowerBeside(u32 upper, u32 lower, const Reg128& upperBefore) {
    const u32 target = getUpperDestination(upper);
    if (target == 0) {
        executeLower(lower);
        return;
    }
    const Reg128 upperResult = mState.vf[target];
    mState.vf[target] = upperBefore;
    executeLower(lower);
    const u32 dest = getDest(upper);
    for (s32 i = 0; i < 4; ++i) {
        if (Vu::writes(dest, i)) {
            mState.vf[target].setWord(i, upperResult.word(i));
        }
    }
}

VuMicro::Result VuMicro::run(u32 start) {
    begin(start);
    for (u32 steps = 0; steps < kRunawayStep; ++steps) {
        if (step()) {
            // The counter names where the program would have gone next, and
            // wraps within the instruction memory like a fetch does.
            return {(mProgramCounter + 1) % getInstructionCount(), true};
        }
    }
    syncPipelines();
    return {mProgramCounter, false};
}

void VuMicro::begin(u32 start) {
    mProgramCounter = start;
    mBranching = false;
    mEnding = false;
    mEndDelay = 1;
    mReadableFlags = {mState.readSeenStatus(), mState.readSeenMac(), mState.vi[Vu::kClip]};
    std::fill(std::begin(mFlagDelayLine), std::end(mFlagDelayLine), mReadableFlags);
    mQuotient = {};
    mFunction = {};
    mCycle = 0;
    for (auto& lanes : mLaneReady) {
        std::fill(std::begin(lanes), std::end(lanes), 0u);
    }
    std::fill(std::begin(mIntegerReady), std::end(mIntegerReady), 0u);
}

bool VuMicro::step() {
    const u32 offset = (mProgramCounter % getInstructionCount()) * 8;
    const u32 lower = readWord(mInstructions, offset);
    const u32 upper = readWord(mInstructions, offset + 4);

    stallUntil(getOperandsReadyCycle(upper, lower));
    beginCycle();

    const bool taken = mBranching;
    const u32 target = mBranchTarget;
    mBranching = false;

    if ((upper & kEndBit) != 0) {
        mEnding = true;
    }
    const Reg128 upperBefore = mState.vf[getUpperDestination(upper)];
    executeUpper(upper);
    if ((upper & kImmediateBit) != 0) {
        mState.vi[Vu::kI] = lower;
    } else {
        executeLowerBeside(upper, lower, upperBefore);
    }
    markWritten(upper, lower);
    endCycle();

    // The end bit stops the program one pair later, so the pair behind it runs.
    if (mEnding && mEndDelay-- == 0) {
        syncPipelines();
        return true;
    }
    mProgramCounter = taken ? target : mProgramCounter + 1;
    return false;
}

bool runMicroProgram(Vu& state, Memory& memory, Gif* gif, u32 unit, u32 start) {
    const VuMicro::Result result = VuMicro(state, memory, unit, gif).run(start);
    state.vi[Vu::kTpc] = result.counter;
    return result.ended;
}

}  // namespace uee
