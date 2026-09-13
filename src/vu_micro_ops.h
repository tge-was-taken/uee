#pragma once

#include "types.h"

#include <string>

namespace uee {

// Defined in vu_handlers.h, which includes this through vu_micro.h.
enum class VuSource;

constexpr u32 kVuEndBit = 1u << 30;
constexpr u32 kVuImmediateBit = 1u << 31;

enum class VuUpperOp : u8 {
    None,
    Add, Sub, Madd, Msub, Max, Mini, Mul,
    Adda, Suba, Madda, Msuba, Mula,
    Opmsub, Opmula, Itof, Ftoi, Abs, Clip, Move,
};

struct VuUpper {
    VuUpperOp op = VuUpperOp::None;
    VuSource source{};
    u32 dest = 0;
    u32 fd = 0;
    u32 fs = 0;
    u32 ft = 0;
    // The fixed point position of itof and ftoi.
    u32 shift = 0;
};

// The elementary functions, by the wide opcode a Function op carries.
namespace elementary {
constexpr u32 kEsadd = 0x73C, kErsadd = 0x73D, kEleng = 0x73E, kErleng = 0x73F;
constexpr u32 kEatanxy = 0x77C, kEatanxz = 0x77D, kEsum = 0x77E;
constexpr u32 kEsqrt = 0x7BC, kErsqrt = 0x7BD, kErcpr = 0x7BE;
constexpr u32 kEsin = 0x7FC, kEatan = 0x7FD, kEexp = 0x7FE;
}  // namespace elementary

enum class VuLowerOp : u8 {
    None,
    // A pair with the I bit carries a constant for I here instead of an op.
    Immediate,
    Move, Mr32, Mfir, Mtir, Rinit, Rxor, Rnext, Rget, Mfp, Xgkick, Xtop, Xitop,
    Function, Div, Sqrt, Rsqrt,
    Ilwr, Iswr, Lqi, Sqi, Lqd, Sqd,
    Iadd, Isub, Iand, Ior, Iaddi,
    Lq, Sq, Fcget, Fcset, Fcand, Fcor, Fceq, Fsand, Fsor, Fsset, Fseq, Fmand, Fmor, Fmeq,
    Iaddiu, Isubiu, Ilw, Isw,
    B, Bal, Jr, Jalr, Ibeq, Ibne, Ibltz, Ibgtz, Iblez, Ibgez,
};

struct VuLower {
    VuLowerOp op = VuLowerOp::None;
    u32 dest = 0;
    u32 fd = 0;
    u32 fs = 0;
    u32 ft = 0;
    // Which lane of fs and ft a scalar op reads.
    u32 fsLane = 0;
    u32 ftLane = 0;
    // The branch offset, or the immediate an op carries; the raw word for the
    // Immediate pseudo-op.
    s32 offset = 0;
    u32 immediate = 0;
    // Which elementary function a Function op computes, as its wide opcode.
    u32 function = 0;
};

VuUpper decodeUpper(u32 upper);
VuLower decodeLower(u32 upper, u32 lower);

// The register lanes one op waits on or leaves pending, for the hazard check.
struct VuLanes {
    u32 vf = 0;
    u32 lanes = 0;
};

// A register of zero stands for no use at all, since vf0 never changes.
struct VuPairTiming {
    VuLanes upperReads[2];
    VuLanes lowerReads[2];
    // Integer registers a branch compares, which wait on an integer load.
    u32 integerReads[2] = {};
    u32 integerReadCount = 0;
    bool waitsForDivider = false;
    bool waitsForFunction = false;
    VuLanes upperWrite;
    VuLanes lowerWrite;
    // The integer register a load fills, if any.
    bool loadsInteger = false;
    u32 integerLoad = 0;
};

VuPairTiming getPairTiming(u32 upper, u32 lower);

// Whether the lower op reads or writes a register, which decides if it has to
// see the register as it was before the upper op of the same pair.
bool touchesRegister(const VuPairTiming& timing, u32 vf);
u32 getUpperDestination(u32 upper);

// Whether the pair can change the status, MAC or clip flags, and whether its
// lower op reads the ones a program sees.
bool changesFlags(const VuUpper& upper, const VuLower& lower);
bool readsFlags(const VuLower& lower);

// The pair as text, for comments in translated code and for logs.
std::string disassembleUpper(u32 upper);
std::string disassembleLower(u32 pc, u32 upper, u32 lower);

// Where a lower op sends control, for anything walking a program.
enum class VuTransfer { None, Jump, Branch, Indirect };
VuTransfer getTransfer(const VuLower& lower);

}  // namespace uee
