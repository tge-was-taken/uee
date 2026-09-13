#pragma once

// The handlers a micro program runs through, whether the interpreter decoded
// the pair or a translated program names them directly. They sit in their own
// namespace since several share a name with an EE handler or a math function.

#include "memory.h"
#include "vu_handlers.h"
#include "vu_micro.h"

#include <algorithm>
#include <cstring>
#include <span>

namespace uee::micro {

// VU0's 4 KB repeat over the 16 KB it decodes and nothing lies past them; VU1
// fills its decode, so every address wraps.
inline bool isAddressable(u32 unit, u32 quadword) {
    constexpr u32 kDecodedQuadwords = 0x400;
    if (unit != 0) {
        return true;
    }
    return static_cast<s16>(quadword) >= 0 && (quadword & 0xFFFFu) < kDecodedQuadwords;
}

inline u32 readWord(std::span<const u8> bytes, u32 offset) {
    u32 value;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

inline void writeWord(std::span<u8> bytes, u32 offset, u32 value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

// Out of line: rare, or too large to repeat at every call.
void syncPipelines(VuMicro& m);
u32 computeFunction(VuMicro& m, u32 function, u32 fs, u32 lane);
u32 getFunctionLatency(u32 function);
u32 takeQuotient(VuMicro& m, Vu::Quotient q);
void kickToGif(VuMicro& m, u32 quadword);
void xtop(VuMicro& m, u32 ft);
void xitop(VuMicro& m, u32 ft);
// Carries on in the interpreter from pc with the pipelines as they are.
VuMicro::Result resume(VuMicro& m, u32 pc);
VuMicro::Result finish(const VuMicro& m);
VuMicro::Result stop(VuMicro& m);

void landResults(VuMicro& m);

// Results land between cycles, so jumping several at once lands them the same.
inline void advanceTo(VuMicro& m, u32 cycle) {
    m.cycle = cycle;
    if (m.cycle >= m.landingCycle) {
        landResults(m);
    }
}

inline void issueResult(VuMicro& m, VuMicro::DelayedResult& result, u32 value, u32 latency) {
    result = {value, m.cycle + latency, true};
    m.landingCycle = std::min(m.quotient.getLandingCycle(), m.function.getLandingCycle());
}

inline void recordFlags(VuMicro& m) {
    m.flagNewest = (m.flagNewest + 1) % VuMicro::kFlagDelay;
    m.flagHistory[m.flagNewest] = {
        m.cycle + VuMicro::kFlagDelay,
        {m.state.vi[Vu::kStatus], m.state.vi[Vu::kMac], m.state.clip}};
}

inline const VuMicro::FlagSet& getReadableFlags(const VuMicro& m) {
    for (u32 i = 0; i + 1 < VuMicro::kFlagDelay; ++i) {
        const VuMicro::FlagRecord& record =
            m.flagHistory[(m.flagNewest + VuMicro::kFlagDelay - i) % VuMicro::kFlagDelay];
        if (record.readyCycle <= m.cycle) {
            return record.flags;
        }
    }
    return m.flagHistory[(m.flagNewest + 1) % VuMicro::kFlagDelay].flags;
}

// The branch the pair before set lands after this one.
inline void beginPair(VuMicro& m) {
    m.taken = m.branching;
    m.takenTarget = m.branchTarget;
    m.branching = false;
}

// The end bit stops the program one pair later, so the pair behind it runs.
inline void markEnding(VuMicro& m) {
    m.ending = true;
}

// True once the program has ended; otherwise the counter names the next pair.
inline bool endPair(VuMicro& m, u32 pc) {
    advanceTo(m, m.cycle + 1);
    if (m.ending && m.endDelay-- == 0) {
        syncPipelines(m);
        return true;
    }
    m.programCounter = m.taken ? m.takenTarget : pc + 1;
    return false;
}

// For pairs that are neither a delay slot nor next to an end bit: no branch
// lands after them and the program cannot end there, so they skip both pair
// ends, and a run of them that reads no cycle count adds its cycles at once.
inline void advanceBy(VuMicro& m, u32 cycles) {
    advanceTo(m, m.cycle + cycles);
}

// Hazards are checked a field at a time, so writing one lane of a register
// holds up only the instructions that read that lane.
inline u32 getLanesReady(const VuMicro& m, u32 vf, u32 lanes) {
    u32 ready = 0;
    for (s32 i = 0; vf != 0 && i < 4; ++i) {
        if (Vu::writes(lanes, i)) {
            ready = std::max(ready, m.laneReady[vf][i]);
        }
    }
    return ready;
}

inline u32 getIntegerReady(const VuMicro& m, u32 vi) {
    return m.integerReady[vi & 0xFu];
}

// The divider and the function unit take one op at a time, so another op or a
// wait holds until the one in flight lands.
inline u32 getDividerFree(const VuMicro& m) {
    return m.quotient.pending ? m.quotient.readyCycle : 0;
}

inline u32 getFunctionFree(const VuMicro& m) {
    return m.function.pending ? m.function.readyCycle : 0;
}

// The op that set a flag keeps moving while the one waiting on it stalls, so
// the flags land after the same number of cycles either way.
inline void stallUntil(VuMicro& m, u32 cycle) {
    if (m.cycle < cycle) {
        advanceTo(m, cycle);
    }
}

// A result reaches its register through the same four stages whichever half
// of the pair produced it.
inline void markLanes(VuMicro& m, u32 vf, u32 lanes) {
    for (s32 i = 0; i < 4; ++i) {
        if (vf != 0 && Vu::writes(lanes, i)) {
            m.laneReady[vf][i] = m.cycle + VuMicro::kFmacLatency;
        }
    }
}

inline void markIntegerLoad(VuMicro& m, u32 vi) {
    m.integerReady[vi & 0xFu] = m.cycle + VuMicro::kIntegerLoadLatency;
}

inline void setImmediate(VuMicro& m, u32 value) {
    m.state.vi[Vu::kI] = value;
}

inline void loadQuadword(VuMicro& m, u32 dest, u32 vf, u32 quadword) {
    if (!isAddressable(m.unit, quadword)) {
        const u32 zero[4]{};
        m.state.writeVf(vf, dest, zero);
        return;
    }
    const u32 offset = (quadword * 16u) & (m.getDataSize() - 1);
    u32 lanes[4];
    for (s32 i = 0; i < 4; ++i) {
        lanes[i] = readWord(m.data, offset + 4u * static_cast<u32>(i));
    }
    m.state.writeVf(vf, dest, lanes);
    if (m.unit == 0) {
        for (s32 i = 0; i < 4; ++i) {
            if (Vu::writes(dest, i)) {
                m.memory.keepVuWindowWord(lanes[i]);
            }
        }
    }
}

inline void storeQuadword(VuMicro& m, u32 dest, u32 vf, u32 quadword) {
    if (!isAddressable(m.unit, quadword)) {
        return;
    }
    const u32 offset = (quadword * 16u) & (m.getDataSize() - 1);
    const Reg128 value = m.state.readVf(vf);
    for (s32 i = 0; i < 4; ++i) {
        if (Vu::writes(dest, i)) {
            writeWord(m.data, offset + 4u * static_cast<u32>(i), value.word(i));
            m.memory.keepVuWindowWord(value.word(i));
        }
    }
}

inline u32 readLane(const VuMicro& m, u32 vf, u32 lane) {
    return m.state.readVf(vf).word(static_cast<s32>(lane));
}

inline void branchTo(VuMicro& m, u32 target) {
    m.branchTarget = target;
    m.branching = true;
}

inline void recordJump(VuMicro& m, u32 target) {
    if (m.jumps != nullptr) {
        m.jumps->insert(target % m.getInstructionCount());
    }
}

inline void lq(VuMicro& m, u32 dest, u32 ft, u32 fs, s32 offset) {
    loadQuadword(m, dest, ft, m.state.readVi(fs) + static_cast<u32>(offset));
}

inline void sq(VuMicro& m, u32 dest, u32 fs, u32 ft, s32 offset) {
    storeQuadword(m, dest, fs, m.state.readVi(ft) + static_cast<u32>(offset));
}

inline void lqi(VuMicro& m, u32 dest, u32 ft, u32 fs) {
    loadQuadword(m, dest, ft, m.state.readVi(fs));
    m.state.writeVi(fs, m.state.readVi(fs) + 1);
}

inline void sqi(VuMicro& m, u32 dest, u32 fs, u32 ft) {
    storeQuadword(m, dest, fs, m.state.readVi(ft));
    m.state.writeVi(ft, m.state.readVi(ft) + 1);
}

inline void lqd(VuMicro& m, u32 dest, u32 ft, u32 fs) {
    m.state.writeVi(fs, m.state.readVi(fs) - 1);
    loadQuadword(m, dest, ft, m.state.readVi(fs));
}

inline void sqd(VuMicro& m, u32 dest, u32 fs, u32 ft) {
    m.state.writeVi(ft, m.state.readVi(ft) - 1);
    storeQuadword(m, dest, fs, m.state.readVi(ft));
}

inline void ilw(VuMicro& m, u32 dest, u32 ft, u32 fs, s32 offset) {
    vilwr(m.state, m.memory, dest, ft, fs, m.getDataBase(), m.getDataSize(), offset);
}

inline void isw(VuMicro& m, u32 dest, u32 ft, u32 fs, s32 offset) {
    viswr(m.state, m.memory, dest, ft, fs, m.getDataBase(), m.getDataSize(), offset);
}

inline void ilwr(VuMicro& m, u32 dest, u32 ft, u32 fs) {
    vilwr(m.state, m.memory, dest, ft, fs, m.getDataBase(), m.getDataSize());
}

inline void iswr(VuMicro& m, u32 dest, u32 ft, u32 fs) {
    viswr(m.state, m.memory, dest, ft, fs, m.getDataBase(), m.getDataSize());
}

inline void fcget(VuMicro& m, u32 ft) {
    m.state.writeVi(ft, getReadableFlags(m).clip & 0xFFFu);
}

inline void fcset(VuMicro& m, u32 immediate) {
    m.state.clip = immediate;
    m.state.vi[Vu::kClip] = immediate;
    for (VuMicro::FlagRecord& record : m.flagHistory) {
        record.flags.clip = immediate;
    }
}

inline void fcand(VuMicro& m, u32 immediate) {
    m.state.writeVi(1, (getReadableFlags(m).clip & immediate) != 0 ? 1u : 0u);
}

inline void fcor(VuMicro& m, u32 immediate) {
    m.state.writeVi(1, (getReadableFlags(m).clip | immediate) == 0xFFFFFFu ? 1u : 0u);
}

inline void fceq(VuMicro& m, u32 immediate) {
    m.state.writeVi(1, getReadableFlags(m).clip == immediate ? 1u : 0u);
}

inline void fsand(VuMicro& m, u32 ft, u32 immediate) {
    m.state.writeVi(ft, getReadableFlags(m).status & immediate);
}

inline void fsor(VuMicro& m, u32 ft, u32 immediate) {
    m.state.writeVi(ft, getReadableFlags(m).status | immediate);
}

// The low bits are a view of the flag register, so a write reaches only the
// sticky copies, the same as ctc2.
inline void fsset(VuMicro& m, u32 immediate) {
    m.state.writeControl(Vu::kStatus, immediate);
}

inline void fseq(VuMicro& m, u32 ft, u32 immediate) {
    m.state.writeVi(ft, getReadableFlags(m).status == immediate ? 1u : 0u);
}

inline void fmand(VuMicro& m, u32 ft, u32 fs) {
    m.state.writeVi(ft, getReadableFlags(m).mac & m.state.readVi(fs));
}

inline void fmor(VuMicro& m, u32 ft, u32 fs) {
    m.state.writeVi(ft, getReadableFlags(m).mac | m.state.readVi(fs));
}

inline void fmeq(VuMicro& m, u32 ft, u32 fs) {
    m.state.writeVi(ft, getReadableFlags(m).mac == m.state.readVi(fs) ? 1u : 0u);
}

inline void iaddiu(VuMicro& m, u32 ft, u32 fs, u32 immediate) {
    m.state.writeVi(ft, m.state.readVi(fs) + immediate);
}

inline void isubiu(VuMicro& m, u32 ft, u32 fs, u32 immediate) {
    m.state.writeVi(ft, m.state.readVi(fs) - immediate);
}

inline void b(VuMicro& m, u32 pc, s32 offset) {
    branchTo(m, pc + 1 + static_cast<u32>(offset));
}

inline void bal(VuMicro& m, u32 pc, u32 ft, s32 offset) {
    m.state.writeVi(ft, pc + 2);
    branchTo(m, pc + 1 + static_cast<u32>(offset));
}

inline void jr(VuMicro& m, u32 fs) {
    recordJump(m, m.state.readVi(fs));
    branchTo(m, m.state.readVi(fs));
}

inline void jalr(VuMicro& m, u32 pc, u32 ft, u32 fs) {
    const u32 target = m.state.readVi(fs);
    recordJump(m, target);
    m.state.writeVi(ft, pc + 2);
    branchTo(m, target);
}

inline void ibeq(VuMicro& m, u32 pc, u32 ft, u32 fs, s32 offset) {
    if (m.state.readVi(ft) == m.state.readVi(fs)) {
        branchTo(m, pc + 1 + static_cast<u32>(offset));
    }
}

inline void ibne(VuMicro& m, u32 pc, u32 ft, u32 fs, s32 offset) {
    if (m.state.readVi(ft) != m.state.readVi(fs)) {
        branchTo(m, pc + 1 + static_cast<u32>(offset));
    }
}

inline void ibltz(VuMicro& m, u32 pc, u32 fs, s32 offset) {
    if (static_cast<s16>(m.state.readVi(fs)) < 0) {
        branchTo(m, pc + 1 + static_cast<u32>(offset));
    }
}

inline void ibgtz(VuMicro& m, u32 pc, u32 fs, s32 offset) {
    if (static_cast<s16>(m.state.readVi(fs)) > 0) {
        branchTo(m, pc + 1 + static_cast<u32>(offset));
    }
}

inline void iblez(VuMicro& m, u32 pc, u32 fs, s32 offset) {
    if (static_cast<s16>(m.state.readVi(fs)) <= 0) {
        branchTo(m, pc + 1 + static_cast<u32>(offset));
    }
}

inline void ibgez(VuMicro& m, u32 pc, u32 fs, s32 offset) {
    if (static_cast<s16>(m.state.readVi(fs)) >= 0) {
        branchTo(m, pc + 1 + static_cast<u32>(offset));
    }
}

inline void rinit(VuMicro& m, u32 fs, u32 lane) {
    vrinit(m.state, readLane(m, fs, lane));
}

inline void rxor(VuMicro& m, u32 fs, u32 lane) {
    vrxor(m.state, readLane(m, fs, lane));
}

inline void advanceRandom(VuMicro& m) {
    u32& value = m.state.vi[Vu::kR];
    const u32 bit = ((value >> 4) ^ (value >> 22)) & 1u;
    value = ((value << 1) | bit) & 0x7FFFFFu;
}

inline void rnext(VuMicro& m, u32 dest, u32 ft) {
    advanceRandom(m);
    vrget(m.state, dest, ft);
}

// The first unit has no P register of its own, so it reads back whatever last
// crossed its memory window.
inline void mfp(VuMicro& m, u32 dest, u32 ft) {
    if (m.unit == 0) {
        m.state.vi[Vu::kP] = m.memory.getVuWindowWord();
    }
    vmfp(m.state, dest, ft);
}

inline void xgkick(VuMicro& m, u32 fs) {
    kickToGif(m, m.state.readVi(fs));
}

// The first unit has no elementary function unit, so the instruction that
// names one does nothing there.
inline void efu(VuMicro& m, u32 function, u32 fs, u32 lane) {
    if (m.unit == 1) {
        issueResult(m, m.function, computeFunction(m, function, fs, lane),
                    getFunctionLatency(function));
    }
}

inline void div(VuMicro& m, u32 fs, u32 fsLane, u32 ft, u32 ftLane) {
    issueResult(m, m.quotient,
                takeQuotient(m, computeVuQuotient(readLane(m, fs, fsLane), readLane(m, ft, ftLane))),
                VuMicro::kQuotientLatency);
}

inline void sqrt(VuMicro& m, u32 ft, u32 ftLane) {
    issueResult(m, m.quotient, takeQuotient(m, computeVuRoot(readLane(m, ft, ftLane))),
                VuMicro::kQuotientLatency);
}

inline void rsqrt(VuMicro& m, u32 fs, u32 fsLane, u32 ft, u32 ftLane) {
    issueResult(m, m.quotient,
                takeQuotient(m, computeVuInverseRoot(readLane(m, fs, fsLane),
                                                     readLane(m, ft, ftLane))),
                VuMicro::kRootLatency);
}

}  // namespace uee::micro
