#include "vu_micro_ops.h"

#include "vu_handlers.h"

#include <algorithm>
#include <format>

namespace uee {
namespace {

constexpr bool isRegisterForm(u32 code) { return (code & 0x80000000u) != 0; }
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
using namespace elementary;
constexpr u32 kWaitp = 0x7BF;
constexpr u32 kXgkick = 0x6FC, kXtop = 0x6BC, kXitop = 0x6BD;
}  // namespace lower

VuUpper makeUpper(u32 code, VuUpperOp op, VuSource source) {
    return {op, source, getDest(code), getFd(code), getFs(code), getFt(code), 0};
}

VuUpper makeFixed(u32 code, VuUpperOp op, u32 shift) {
    VuUpper upper = makeUpper(code, op, VuSource::Vt);
    upper.shift = shift;
    return upper;
}

VuSource getWideSource(u32 wide) {
    switch (wide & 3u) {
        case 0: return VuSource::X;
        case 1: return VuSource::Y;
        case 2: return VuSource::Z;
        default: return VuSource::W;
    }
}

}  // namespace

// The vector register an upper instruction writes, or zero for one that writes
// only the accumulator, the clip flags or nothing.
u32 getUpperDestination(u32 code) {
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

VuUpper decodeUpper(u32 code) {
    const u32 opcode = code & 0x3Fu;
    switch (opcode >> 2) {
        case 0x0: return makeUpper(code, VuUpperOp::Add, getBroadcastSource(code));
        case 0x1: return makeUpper(code, VuUpperOp::Sub, getBroadcastSource(code));
        case 0x2: return makeUpper(code, VuUpperOp::Madd, getBroadcastSource(code));
        case 0x3: return makeUpper(code, VuUpperOp::Msub, getBroadcastSource(code));
        case 0x4: return makeUpper(code, VuUpperOp::Max, getBroadcastSource(code));
        case 0x5: return makeUpper(code, VuUpperOp::Mini, getBroadcastSource(code));
        case 0x6: return makeUpper(code, VuUpperOp::Mul, getBroadcastSource(code));
        default: break;
    }
    switch (opcode) {
        case 0x1C: return makeUpper(code, VuUpperOp::Mul, VuSource::Q);
        case 0x1D: return makeUpper(code, VuUpperOp::Max, VuSource::I);
        case 0x1E: return makeUpper(code, VuUpperOp::Mul, VuSource::I);
        case 0x1F: return makeUpper(code, VuUpperOp::Mini, VuSource::I);
        case 0x20: return makeUpper(code, VuUpperOp::Add, VuSource::Q);
        case 0x21: return makeUpper(code, VuUpperOp::Madd, VuSource::Q);
        case 0x22: return makeUpper(code, VuUpperOp::Add, VuSource::I);
        case 0x23: return makeUpper(code, VuUpperOp::Madd, VuSource::I);
        case 0x24: return makeUpper(code, VuUpperOp::Sub, VuSource::Q);
        case 0x25: return makeUpper(code, VuUpperOp::Msub, VuSource::Q);
        case 0x26: return makeUpper(code, VuUpperOp::Sub, VuSource::I);
        case 0x27: return makeUpper(code, VuUpperOp::Msub, VuSource::I);
        case 0x28: return makeUpper(code, VuUpperOp::Add, VuSource::Vt);
        case 0x29: return makeUpper(code, VuUpperOp::Madd, VuSource::Vt);
        case 0x2A: return makeUpper(code, VuUpperOp::Mul, VuSource::Vt);
        case 0x2B: return makeUpper(code, VuUpperOp::Max, VuSource::Vt);
        case 0x2C: return makeUpper(code, VuUpperOp::Sub, VuSource::Vt);
        case 0x2D: return makeUpper(code, VuUpperOp::Msub, VuSource::Vt);
        case 0x2E: return makeUpper(code, VuUpperOp::Opmsub, VuSource::Vt);
        case 0x2F: return makeUpper(code, VuUpperOp::Mini, VuSource::Vt);
        default: break;
    }

    const u32 wide = getWideOpcode(code);
    switch (wide) {
        case 0x03C: case 0x03D: case 0x03E: case 0x03F:
            return makeUpper(code, VuUpperOp::Adda, getWideSource(wide));
        case 0x07C: case 0x07D: case 0x07E: case 0x07F:
            return makeUpper(code, VuUpperOp::Suba, getWideSource(wide));
        case 0x0BC: case 0x0BD: case 0x0BE: case 0x0BF:
            return makeUpper(code, VuUpperOp::Madda, getWideSource(wide));
        case 0x0FC: case 0x0FD: case 0x0FE: case 0x0FF:
            return makeUpper(code, VuUpperOp::Msuba, getWideSource(wide));
        case 0x1BC: case 0x1BD: case 0x1BE: case 0x1BF:
            return makeUpper(code, VuUpperOp::Mula, getWideSource(wide));
        case 0x1FC: return makeUpper(code, VuUpperOp::Mula, VuSource::Q);
        case 0x1FE: return makeUpper(code, VuUpperOp::Mula, VuSource::I);
        case 0x23C: return makeUpper(code, VuUpperOp::Adda, VuSource::Q);
        case 0x23D: return makeUpper(code, VuUpperOp::Madda, VuSource::Q);
        case 0x23E: return makeUpper(code, VuUpperOp::Adda, VuSource::I);
        case 0x23F: return makeUpper(code, VuUpperOp::Madda, VuSource::I);
        case 0x27C: return makeUpper(code, VuUpperOp::Suba, VuSource::Q);
        case 0x27D: return makeUpper(code, VuUpperOp::Msuba, VuSource::Q);
        case 0x27E: return makeUpper(code, VuUpperOp::Suba, VuSource::I);
        case 0x27F: return makeUpper(code, VuUpperOp::Msuba, VuSource::I);
        case 0x2BC: return makeUpper(code, VuUpperOp::Adda, VuSource::Vt);
        case 0x2BD: return makeUpper(code, VuUpperOp::Madda, VuSource::Vt);
        case 0x2BE: return makeUpper(code, VuUpperOp::Mula, VuSource::Vt);
        case 0x2FC: return makeUpper(code, VuUpperOp::Suba, VuSource::Vt);
        case 0x2FD: return makeUpper(code, VuUpperOp::Msuba, VuSource::Vt);
        case 0x13C: return makeFixed(code, VuUpperOp::Itof, 0);
        case 0x13D: return makeFixed(code, VuUpperOp::Itof, 4);
        case 0x13E: return makeFixed(code, VuUpperOp::Itof, 12);
        case 0x13F: return makeFixed(code, VuUpperOp::Itof, 15);
        case 0x17C: return makeFixed(code, VuUpperOp::Ftoi, 0);
        case 0x17D: return makeFixed(code, VuUpperOp::Ftoi, 4);
        case 0x17E: return makeFixed(code, VuUpperOp::Ftoi, 12);
        case 0x17F: return makeFixed(code, VuUpperOp::Ftoi, 15);
        case 0x1FD: return makeUpper(code, VuUpperOp::Abs, VuSource::Vt);
        case 0x1FF: return makeUpper(code, VuUpperOp::Clip, VuSource::Vt);
        case 0x2FE: return makeUpper(code, VuUpperOp::Opmula, VuSource::Vt);
        case 0x33C: return makeUpper(code, VuUpperOp::Move, VuSource::Vt);
        default: return {};
    }
}

VuLower decodeLower(u32 upper, u32 code) {
    VuLower decoded;
    if ((upper & kVuImmediateBit) != 0) {
        decoded.op = VuLowerOp::Immediate;
        decoded.immediate = code;
        return decoded;
    }
    decoded.dest = getDest(code);
    decoded.fd = getFd(code);
    decoded.fs = getFs(code);
    decoded.ft = getFt(code);
    decoded.fsLane = (code >> 21) & 3u;
    decoded.ftLane = (code >> 23) & 3u;
    const auto with = [&](VuLowerOp op) {
        decoded.op = op;
        return decoded;
    };
    // A register write to vf0 or to no lanes does nothing, and the lower nop is
    // encoded as one.
    const auto withWrite = [&](VuLowerOp op) {
        return with(decoded.ft == 0 || decoded.dest == 0 ? VuLowerOp::None : op);
    };

    if (isRegisterForm(code)) {
        if (!isWide(code)) {
            switch (code & 0x3Fu) {
                case lower::kIadd: return with(VuLowerOp::Iadd);
                case lower::kIsub: return with(VuLowerOp::Isub);
                case lower::kIand: return with(VuLowerOp::Iand);
                case lower::kIor: return with(VuLowerOp::Ior);
                case lower::kIaddi:
                    decoded.offset = getSigned5(code);
                    return with(VuLowerOp::Iaddi);
                default: return with(VuLowerOp::None);
            }
        }
        const u32 wide = getWideOpcode(code);
        switch (wide) {
            case lower::kMove: return withWrite(VuLowerOp::Move);
            case lower::kMr32: return withWrite(VuLowerOp::Mr32);
            case lower::kMfir: return withWrite(VuLowerOp::Mfir);
            case lower::kMtir: return with(VuLowerOp::Mtir);
            case lower::kRinit: return with(VuLowerOp::Rinit);
            case lower::kRxor: return with(VuLowerOp::Rxor);
            case lower::kRnext: return with(VuLowerOp::Rnext);
            case lower::kRget: return withWrite(VuLowerOp::Rget);
            case lower::kMfp: return with(VuLowerOp::Mfp);
            case lower::kXgkick: return with(VuLowerOp::Xgkick);
            case lower::kXtop: return with(VuLowerOp::Xtop);
            case lower::kXitop: return with(VuLowerOp::Xitop);
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
                decoded.function = wide;
                return with(VuLowerOp::Function);
            case lower::kDiv: return with(VuLowerOp::Div);
            case lower::kSqrt: return with(VuLowerOp::Sqrt);
            case lower::kRsqrt: return with(VuLowerOp::Rsqrt);
            case lower::kIlwr: return with(VuLowerOp::Ilwr);
            case lower::kIswr: return with(VuLowerOp::Iswr);
            case lower::kLqi: return with(VuLowerOp::Lqi);
            case lower::kSqi: return with(VuLowerOp::Sqi);
            case lower::kLqd: return with(VuLowerOp::Lqd);
            case lower::kSqd: return with(VuLowerOp::Sqd);
            // WAITQ and WAITP only wait, which the timing already covers.
            default: return with(VuLowerOp::None);
        }
    }

    decoded.offset = getSigned11(code);
    const u32 immediate24 = code & 0xFFFFFFu;
    switch ((code >> 25) & 0x7Fu) {
        case lower::kLq: return with(VuLowerOp::Lq);
        case lower::kSq: return with(VuLowerOp::Sq);
        case lower::kFcget: return with(VuLowerOp::Fcget);
        case lower::kFcset:
            decoded.immediate = immediate24;
            return with(VuLowerOp::Fcset);
        case lower::kFcand:
            decoded.immediate = immediate24;
            return with(VuLowerOp::Fcand);
        case lower::kFcor:
            decoded.immediate = immediate24;
            return with(VuLowerOp::Fcor);
        case lower::kFceq:
            decoded.immediate = immediate24;
            return with(VuLowerOp::Fceq);
        case lower::kFsand:
            decoded.immediate = getStatusImmediate(code);
            return with(VuLowerOp::Fsand);
        case lower::kFsor:
            decoded.immediate = getStatusImmediate(code);
            return with(VuLowerOp::Fsor);
        case lower::kFsset:
            decoded.immediate = getStatusImmediate(code);
            return with(VuLowerOp::Fsset);
        case lower::kFseq:
            decoded.immediate = getStatusImmediate(code);
            return with(VuLowerOp::Fseq);
        case lower::kFmand: return with(VuLowerOp::Fmand);
        case lower::kFmor: return with(VuLowerOp::Fmor);
        case lower::kFmeq: return with(VuLowerOp::Fmeq);
        case lower::kIaddiu:
            decoded.immediate = getImmediate15(code);
            return with(VuLowerOp::Iaddiu);
        case lower::kIsubiu:
            decoded.immediate = getImmediate15(code);
            return with(VuLowerOp::Isubiu);
        case lower::kIlw: return with(VuLowerOp::Ilw);
        case lower::kIsw: return with(VuLowerOp::Isw);
        case lower::kB: return with(VuLowerOp::B);
        case lower::kBal: return with(VuLowerOp::Bal);
        case lower::kJr: return with(VuLowerOp::Jr);
        case lower::kJalr: return with(VuLowerOp::Jalr);
        case lower::kIbeq: return with(VuLowerOp::Ibeq);
        case lower::kIbne: return with(VuLowerOp::Ibne);
        case lower::kIbltz: return with(VuLowerOp::Ibltz);
        case lower::kIbgtz: return with(VuLowerOp::Ibgtz);
        case lower::kIblez: return with(VuLowerOp::Iblez);
        case lower::kIbgez: return with(VuLowerOp::Ibgez);
        default: return with(VuLowerOp::None);
    }
}

namespace {

// Hazards are checked a field at a time, so writing one lane of a register
// holds up only the instructions that read that lane.
void describeUpperReads(u32 code, VuLanes reads[2]) {
    const u32 dest = getDest(code);
    const auto readsFrom = [&](u32 fsLanes, u32 ftLanes) {
        reads[0] = {getFs(code), fsLanes};
        reads[1] = {getFt(code), ftLanes};
    };
    const u32 opcode = code & 0x3Fu;
    if (opcode < 0x1C) {
        readsFrom(dest, getLaneMask(getBroadcast(code)));
        return;
    }
    if (opcode < 0x28) {
        readsFrom(dest, 0);  // the Q and I forms
        return;
    }
    if (opcode == 0x2E) {
        readsFrom(kXyz, kXyz);  // OPMSUB
        return;
    }
    if (opcode < 0x30) {
        readsFrom(dest, dest);
        return;
    }
    if (!isWide(code)) {
        return;
    }
    switch (getWideOpcode(code)) {
        case 0x1FF: readsFrom(kXyz, kW); return;    // CLIP
        case 0x2FE: readsFrom(kXyz, kXyz); return;  // OPMULA
        case 0x2BF:
        case 0x2FF: return;
        default: break;
    }
    switch (getWideOpcode(code) >> 6) {
        case 0: case 1: case 2: case 3: case 6:
            readsFrom(dest, getLaneMask(getBroadcast(code)));
            return;
        case 10: case 11:
            readsFrom(dest, dest);
            return;
        default:
            readsFrom(dest, 0);
            return;
    }
}

void describeLowerReads(u32 code, VuPairTiming& timing) {
    const u32 dest = getDest(code), fs = getFs(code), ft = getFt(code);
    const u32 fsField = getLaneMask((code >> 21) & 3u);
    const u32 ftField = getLaneMask((code >> 23) & 3u);
    const auto integers = [&](std::initializer_list<u32> registers) {
        for (const u32 vi : registers) {
            timing.integerReads[timing.integerReadCount++] = vi & 0xFu;
        }
    };
    if (!isRegisterForm(code)) {
        switch ((code >> 25) & 0x7Fu) {
            case lower::kSq: timing.lowerReads[0] = {fs, dest}; return;
            case lower::kIbeq:
            case lower::kIbne:
                integers({fs, ft});
                return;
            case lower::kIbltz:
            case lower::kIbgtz:
            case lower::kIblez:
            case lower::kIbgez:
            case lower::kJr:
            case lower::kJalr:
                integers({fs});
                return;
            default: return;
        }
    }
    if (!isWide(code)) {
        return;
    }
    // The divider and the function unit take one op at a time, so another op
    // or a wait holds until the one in flight lands.
    switch (getWideOpcode(code)) {
        case lower::kMove:
        case lower::kSqi:
        case lower::kSqd:
            timing.lowerReads[0] = {fs, dest};
            return;
        case lower::kMr32:
            timing.lowerReads[0] = {fs, (dest >> 1) | ((dest << 3) & 8u)};
            return;
        case lower::kMtir:
        case lower::kRinit:
        case lower::kRxor:
            timing.lowerReads[0] = {fs, fsField};
            return;
        case lower::kDiv:
        case lower::kRsqrt:
            timing.lowerReads[0] = {fs, fsField};
            timing.lowerReads[1] = {ft, ftField};
            timing.waitsForDivider = true;
            return;
        case lower::kSqrt:
            timing.lowerReads[0] = {ft, ftField};
            timing.waitsForDivider = true;
            return;
        case lower::kWaitq: timing.waitsForDivider = true; return;
        case lower::kEsadd:
        case lower::kErsadd:
        case lower::kEleng:
        case lower::kErleng:
            timing.lowerReads[0] = {fs, kXyz};
            timing.waitsForFunction = true;
            return;
        case lower::kEatanxy:
            timing.lowerReads[0] = {fs, 0xCu};
            timing.waitsForFunction = true;
            return;
        case lower::kEatanxz:
            timing.lowerReads[0] = {fs, 0xAu};
            timing.waitsForFunction = true;
            return;
        case lower::kEsum:
            timing.lowerReads[0] = {fs, 0xFu};
            timing.waitsForFunction = true;
            return;
        case lower::kEsqrt:
        case lower::kErsqrt:
        case lower::kErcpr:
        case lower::kEsin:
        case lower::kEatan:
        case lower::kEexp:
            timing.lowerReads[0] = {fs, fsField};
            timing.waitsForFunction = true;
            return;
        case lower::kWaitp: timing.waitsForFunction = true; return;
        default: return;
    }
}

// A result reaches its register through the same four stages whichever half
// of the pair produced it.
void describeLowerWrites(u32 code, VuPairTiming& timing) {
    if (!isRegisterForm(code)) {
        const u32 opcode = (code >> 25) & 0x7Fu;
        if (opcode == lower::kLq) {
            timing.lowerWrite = {getFt(code), getDest(code)};
        } else if (opcode == lower::kIlw) {
            timing.loadsInteger = true;
            timing.integerLoad = getFt(code) & 0xFu;
        }
        return;
    }
    if (!isWide(code)) {
        return;
    }
    switch (getWideOpcode(code)) {
        case lower::kLqi:
        case lower::kLqd:
        case lower::kMove:
        case lower::kMr32:
        case lower::kMfir:
        case lower::kRget:
        case lower::kRnext:
        case lower::kMfp:
            timing.lowerWrite = {getFt(code), getDest(code)};
            return;
        case lower::kIlwr:
            timing.loadsInteger = true;
            timing.integerLoad = getFt(code) & 0xFu;
            return;
        default: return;
    }
}

}  // namespace

VuPairTiming getPairTiming(u32 upper, u32 lower) {
    VuPairTiming timing;
    describeUpperReads(upper, timing.upperReads);
    timing.upperWrite = {getUpperDestination(upper), getDest(upper)};
    if ((upper & kVuImmediateBit) == 0) {
        describeLowerReads(lower, timing);
        describeLowerWrites(lower, timing);
    }
    return timing;
}

bool touchesRegister(const VuPairTiming& timing, u32 vf) {
    return timing.lowerReads[0].vf == vf || timing.lowerReads[1].vf == vf ||
           timing.lowerWrite.vf == vf;
}

bool changesFlags(const VuUpper& upper, const VuLower& lower) {
    switch (upper.op) {
        case VuUpperOp::None:
        case VuUpperOp::Max:
        case VuUpperOp::Mini:
        case VuUpperOp::Itof:
        case VuUpperOp::Ftoi:
        case VuUpperOp::Abs:
        case VuUpperOp::Move:
            break;
        default: return true;
    }
    switch (lower.op) {
        case VuLowerOp::Div:
        case VuLowerOp::Sqrt:
        case VuLowerOp::Rsqrt:
        case VuLowerOp::Fsset:
        case VuLowerOp::Fcset:
            return true;
        default: return false;
    }
}

bool readsFlags(const VuLower& lower) {
    switch (lower.op) {
        case VuLowerOp::Fcget:
        case VuLowerOp::Fcand:
        case VuLowerOp::Fcor:
        case VuLowerOp::Fceq:
        case VuLowerOp::Fsand:
        case VuLowerOp::Fsor:
        case VuLowerOp::Fseq:
        case VuLowerOp::Fmand:
        case VuLowerOp::Fmor:
        case VuLowerOp::Fmeq:
            return true;
        default: return false;
    }
}

VuTransfer getTransfer(const VuLower& lower) {
    switch (lower.op) {
        case VuLowerOp::B:
        case VuLowerOp::Bal:
            return VuTransfer::Jump;
        case VuLowerOp::Ibeq:
        case VuLowerOp::Ibne:
        case VuLowerOp::Ibltz:
        case VuLowerOp::Ibgtz:
        case VuLowerOp::Iblez:
        case VuLowerOp::Ibgez:
            return VuTransfer::Branch;
        case VuLowerOp::Jr:
        case VuLowerOp::Jalr:
            return VuTransfer::Indirect;
        default:
            return VuTransfer::None;
    }
}

namespace {

std::string getDestSuffix(u32 dest) {
    std::string text = dest == 0 ? "" : ".";
    for (s32 i = 0; i < 4; ++i) {
        if (Vu::writes(dest, i)) {
            text += "xyzw"[i];
        }
    }
    return text;
}

char getLaneName(u32 lane) {
    return "xyzw"[lane & 3u];
}

std::string_view getUpperName(VuUpperOp op) {
    switch (op) {
        case VuUpperOp::Add: return "add";
        case VuUpperOp::Sub: return "sub";
        case VuUpperOp::Madd: return "madd";
        case VuUpperOp::Msub: return "msub";
        case VuUpperOp::Max: return "max";
        case VuUpperOp::Mini: return "mini";
        case VuUpperOp::Mul: return "mul";
        case VuUpperOp::Adda: return "adda";
        case VuUpperOp::Suba: return "suba";
        case VuUpperOp::Madda: return "madda";
        case VuUpperOp::Msuba: return "msuba";
        case VuUpperOp::Mula: return "mula";
        case VuUpperOp::Opmsub: return "opmsub";
        case VuUpperOp::Opmula: return "opmula";
        case VuUpperOp::Itof: return "itof";
        case VuUpperOp::Ftoi: return "ftoi";
        case VuUpperOp::Abs: return "abs";
        case VuUpperOp::Clip: return "clip";
        case VuUpperOp::Move: return "move";
        case VuUpperOp::None: return "nop";
    }
    return "nop";
}

std::string_view getSourceSuffix(VuSource source) {
    switch (source) {
        case VuSource::X: return "x";
        case VuSource::Y: return "y";
        case VuSource::Z: return "z";
        case VuSource::W: return "w";
        case VuSource::Q: return "q";
        case VuSource::I: return "i";
        default: return "";
    }
}

std::string getSourceOperand(const VuUpper& u) {
    switch (u.source) {
        case VuSource::Q: return "q";
        case VuSource::I: return "i";
        case VuSource::Vt: return std::format("vf{}", u.ft);
        default: return std::format("vf{}{}", u.ft, getSourceSuffix(u.source));
    }
}

std::string_view getFunctionName(u32 function) {
    switch (function) {
        case elementary::kEsadd: return "esadd";
        case elementary::kErsadd: return "ersadd";
        case elementary::kEleng: return "eleng";
        case elementary::kErleng: return "erleng";
        case elementary::kEatanxy: return "eatanxy";
        case elementary::kEatanxz: return "eatanxz";
        case elementary::kEsum: return "esum";
        case elementary::kEsqrt: return "esqrt";
        case elementary::kErsqrt: return "ersqrt";
        case elementary::kErcpr: return "ercpr";
        case elementary::kEsin: return "esin";
        case elementary::kEatan: return "eatan";
        default: return "eexp";
    }
}

}  // namespace

std::string disassembleUpper(u32 upper) {
    const VuUpper u = decodeUpper(upper);
    const std::string name = std::string(getUpperName(u.op)) + getDestSuffix(u.dest);
    switch (u.op) {
        case VuUpperOp::None: return "nop";
        case VuUpperOp::Add:
        case VuUpperOp::Sub:
        case VuUpperOp::Madd:
        case VuUpperOp::Msub:
        case VuUpperOp::Max:
        case VuUpperOp::Mini:
        case VuUpperOp::Mul:
            return std::format("{}{}{} vf{}, vf{}, {}", getUpperName(u.op),
                               getSourceSuffix(u.source), getDestSuffix(u.dest), u.fd, u.fs,
                               getSourceOperand(u));
        case VuUpperOp::Adda:
        case VuUpperOp::Suba:
        case VuUpperOp::Madda:
        case VuUpperOp::Msuba:
        case VuUpperOp::Mula:
            return std::format("{}{}{} acc, vf{}, {}", getUpperName(u.op),
                               getSourceSuffix(u.source), getDestSuffix(u.dest), u.fs,
                               getSourceOperand(u));
        case VuUpperOp::Opmsub: return std::format("{} vf{}, vf{}, vf{}", name, u.fd, u.fs, u.ft);
        case VuUpperOp::Opmula: return std::format("{} acc, vf{}, vf{}", name, u.fs, u.ft);
        case VuUpperOp::Itof:
        case VuUpperOp::Ftoi:
            return std::format("{}{}{} vf{}, vf{}", getUpperName(u.op), u.shift,
                               getDestSuffix(u.dest), u.ft, u.fs);
        case VuUpperOp::Abs:
        case VuUpperOp::Move:
            return std::format("{} vf{}, vf{}", name, u.ft, u.fs);
        case VuUpperOp::Clip: return std::format("clip vf{}xyz, vf{}w", u.fs, u.ft);
    }
    return "nop";
}

std::string disassembleLower(u32 pc, u32 upper, u32 lower) {
    const VuLower l = decodeLower(upper, lower);
    const std::string dest = getDestSuffix(l.dest);
    const u32 target = (pc + 1 + static_cast<u32>(l.offset)) & 0xFFFFu;
    switch (l.op) {
        case VuLowerOp::None: return "nop";
        case VuLowerOp::Immediate: return std::format("loi 0x{:08x}", l.immediate);
        case VuLowerOp::Move: return std::format("move{} vf{}, vf{}", dest, l.ft, l.fs);
        case VuLowerOp::Mr32: return std::format("mr32{} vf{}, vf{}", dest, l.ft, l.fs);
        case VuLowerOp::Mfir: return std::format("mfir{} vf{}, vi{}", dest, l.ft, l.fs);
        case VuLowerOp::Mtir:
            return std::format("mtir vi{}, vf{}{}", l.ft, l.fs, getLaneName(l.fsLane));
        case VuLowerOp::Rinit: return std::format("rinit r, vf{}{}", l.fs, getLaneName(l.fsLane));
        case VuLowerOp::Rxor: return std::format("rxor r, vf{}{}", l.fs, getLaneName(l.fsLane));
        case VuLowerOp::Rnext: return std::format("rnext{} vf{}, r", dest, l.ft);
        case VuLowerOp::Rget: return std::format("rget{} vf{}, r", dest, l.ft);
        case VuLowerOp::Mfp: return std::format("mfp{} vf{}, p", dest, l.ft);
        case VuLowerOp::Xgkick: return std::format("xgkick vi{}", l.fs);
        case VuLowerOp::Xtop: return std::format("xtop vi{}", l.ft);
        case VuLowerOp::Xitop: return std::format("xitop vi{}", l.ft);
        case VuLowerOp::Function:
            return std::format("{} p, vf{}{}", getFunctionName(l.function), l.fs,
                               getLaneName(l.fsLane));
        case VuLowerOp::Div:
            return std::format("div q, vf{}{}, vf{}{}", l.fs, getLaneName(l.fsLane), l.ft,
                               getLaneName(l.ftLane));
        case VuLowerOp::Sqrt: return std::format("sqrt q, vf{}{}", l.ft, getLaneName(l.ftLane));
        case VuLowerOp::Rsqrt:
            return std::format("rsqrt q, vf{}{}, vf{}{}", l.fs, getLaneName(l.fsLane), l.ft,
                               getLaneName(l.ftLane));
        case VuLowerOp::Ilwr: return std::format("ilwr{} vi{}, (vi{})", dest, l.ft, l.fs);
        case VuLowerOp::Iswr: return std::format("iswr{} vi{}, (vi{})", dest, l.ft, l.fs);
        case VuLowerOp::Lqi: return std::format("lqi{} vf{}, (vi{}++)", dest, l.ft, l.fs);
        case VuLowerOp::Sqi: return std::format("sqi{} vf{}, (vi{}++)", dest, l.fs, l.ft);
        case VuLowerOp::Lqd: return std::format("lqd{} vf{}, (--vi{})", dest, l.ft, l.fs);
        case VuLowerOp::Sqd: return std::format("sqd{} vf{}, (--vi{})", dest, l.fs, l.ft);
        case VuLowerOp::Iadd: return std::format("iadd vi{}, vi{}, vi{}", l.fd, l.fs, l.ft);
        case VuLowerOp::Isub: return std::format("isub vi{}, vi{}, vi{}", l.fd, l.fs, l.ft);
        case VuLowerOp::Iand: return std::format("iand vi{}, vi{}, vi{}", l.fd, l.fs, l.ft);
        case VuLowerOp::Ior: return std::format("ior vi{}, vi{}, vi{}", l.fd, l.fs, l.ft);
        case VuLowerOp::Iaddi: return std::format("iaddi vi{}, vi{}, {}", l.ft, l.fs, l.offset);
        case VuLowerOp::Lq: return std::format("lq{} vf{}, {}(vi{})", dest, l.ft, l.offset, l.fs);
        case VuLowerOp::Sq: return std::format("sq{} vf{}, {}(vi{})", dest, l.fs, l.offset, l.ft);
        case VuLowerOp::Fcget: return std::format("fcget vi{}", l.ft);
        case VuLowerOp::Fcset: return std::format("fcset 0x{:x}", l.immediate);
        case VuLowerOp::Fcand: return std::format("fcand vi1, 0x{:x}", l.immediate);
        case VuLowerOp::Fcor: return std::format("fcor vi1, 0x{:x}", l.immediate);
        case VuLowerOp::Fceq: return std::format("fceq vi1, 0x{:x}", l.immediate);
        case VuLowerOp::Fsand: return std::format("fsand vi{}, 0x{:x}", l.ft, l.immediate);
        case VuLowerOp::Fsor: return std::format("fsor vi{}, 0x{:x}", l.ft, l.immediate);
        case VuLowerOp::Fsset: return std::format("fsset 0x{:x}", l.immediate);
        case VuLowerOp::Fseq: return std::format("fseq vi{}, 0x{:x}", l.ft, l.immediate);
        case VuLowerOp::Fmand: return std::format("fmand vi{}, vi{}", l.ft, l.fs);
        case VuLowerOp::Fmor: return std::format("fmor vi{}, vi{}", l.ft, l.fs);
        case VuLowerOp::Fmeq: return std::format("fmeq vi{}, vi{}", l.ft, l.fs);
        case VuLowerOp::Iaddiu:
            return std::format("iaddiu vi{}, vi{}, 0x{:x}", l.ft, l.fs, l.immediate);
        case VuLowerOp::Isubiu:
            return std::format("isubiu vi{}, vi{}, 0x{:x}", l.ft, l.fs, l.immediate);
        case VuLowerOp::Ilw: return std::format("ilw{} vi{}, {}(vi{})", dest, l.ft, l.offset, l.fs);
        case VuLowerOp::Isw: return std::format("isw{} vi{}, {}(vi{})", dest, l.ft, l.offset, l.fs);
        case VuLowerOp::B: return std::format("b 0x{:04x}", target);
        case VuLowerOp::Bal: return std::format("bal vi{}, 0x{:04x}", l.ft, target);
        case VuLowerOp::Jr: return std::format("jr vi{}", l.fs);
        case VuLowerOp::Jalr: return std::format("jalr vi{}, vi{}", l.ft, l.fs);
        case VuLowerOp::Ibeq: return std::format("ibeq vi{}, vi{}, 0x{:04x}", l.ft, l.fs, target);
        case VuLowerOp::Ibne: return std::format("ibne vi{}, vi{}, 0x{:04x}", l.ft, l.fs, target);
        case VuLowerOp::Ibltz: return std::format("ibltz vi{}, 0x{:04x}", l.fs, target);
        case VuLowerOp::Ibgtz: return std::format("ibgtz vi{}, 0x{:04x}", l.fs, target);
        case VuLowerOp::Iblez: return std::format("iblez vi{}, 0x{:04x}", l.fs, target);
        case VuLowerOp::Ibgez: return std::format("ibgez vi{}, 0x{:04x}", l.fs, target);
    }
    return "nop";
}

}  // namespace uee
