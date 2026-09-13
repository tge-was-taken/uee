#pragma once

#include "types.h"
#include "vu_micro.h"

#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <utility>
#include <vector>

namespace uee {

class VuCapture;

// A run of instruction pairs, counted in pairs.
struct VuPairRange {
    u32 first;
    u32 count;
};

// A micro program translated ahead of time. It stands for the program only
// while micro memory still holds the pairs it was made from.
struct TranslatedVuProgram {
    u32 unit;
    u32 start;
    std::span<const VuPairRange> pairs;
    u64 hash;
    VuMicro::Result (*run)(VuMicro& micro);
};

// The same at translation time and at run time.
u64 hashVuPairs(std::span<const u8> micro, std::span<const VuPairRange> pairs);

// One program: the code a start can reach in one version of micro memory, as
// it was, where each indirect jump was seen to go, and which pairs actually ran.
// The reachable code is the program and its identity; what ran is what a
// specialized translation builds on.
struct VuProgramRecord {
    u32 unit = 0;
    u32 start = 0;
    // The unit's whole instruction memory; only reached pairs mean anything.
    std::vector<u8> code;
    std::vector<bool> reached;
    std::vector<bool> covered;
    std::map<u32, std::set<u32>> jumps;

    bool matches(std::span<const u8> micro) const;
    void reach(u32 from, std::span<const u8> micro);
    void cover(u32 pc, std::span<const u8> micro);
    void addJump(u32 site, u32 target, std::span<const u8> micro);
    std::vector<VuPairRange> getRanges() const;
    std::vector<VuPairRange> getCoveredRanges() const;
};

class VuProgramLog {
public:
    // The record a run from this start continues: the one whose pairs micro
    // memory still holds, or a new one when the code there changed.
    VuProgramRecord& find(u32 unit, u32 start, std::span<const u8> micro);

    // Adds what a file already holds, so runs accumulate.
    void load(const std::filesystem::path& path);
    void save(const std::filesystem::path& path) const;

    std::vector<const VuProgramRecord*> getRecords() const;

private:
    // The record that last matched first: a game runs the same code at a start
    // many times before it uploads other code there.
    std::map<std::pair<u32, u32>, std::vector<std::unique_ptr<VuProgramRecord>>> mRecords;
};

// Runs a translation for a start when one matches micro memory, and the
// interpreter otherwise. Checking runs the interpreter first on a copy and
// reports where the translation ended up elsewhere.
class VuPrograms {
public:
    VuPrograms(std::span<const TranslatedVuProgram> translated, VuProgramLog* log,
               VuCapture* capture, bool check);

    VuMicro::Result run(Vu& state, Memory& memory, Gif* gif, u32 unit, u32 start);

private:
    VuMicro::Result runProgram(VuMicro& micro, u32 start);
    const TranslatedVuProgram* find(u32 unit, u32 start, std::span<const u8> micro);
    VuMicro::Result runChecked(const TranslatedVuProgram& program, Vu& state, Memory& memory,
                               Gif* gif, u32 start);

    // The candidates for each start, the one that last matched first: a game
    // runs the same program at a start many times before it uploads another.
    std::map<std::pair<u32, u32>, std::vector<const TranslatedVuProgram*>> mByStart;

    VuProgramLog* mLog;
    VuCapture* mCapture;
    bool mCheck;
};

}  // namespace uee
