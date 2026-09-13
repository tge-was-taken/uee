#pragma once

#include "types.h"
#include "vu_micro.h"

#include <filesystem>
#include <map>
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

// What the interpreter ran, kept so it can be translated later: each distinct
// micro memory image, the starts run against it and where its indirect jumps
// landed.
class VuProgramLog {
public:
    struct Image {
        u32 unit = 0;
        std::vector<u8> bytes;
        std::set<u32> starts;
        std::set<u32> jumps;
    };
    using Key = std::pair<u32, u64>;

    // The image the run belongs to, which is where its jumps are recorded.
    Image& recordStart(u32 unit, std::span<const u8> micro, u32 start);

    // Adds what a file already holds, so runs accumulate.
    void load(const std::filesystem::path& path);
    void save(const std::filesystem::path& path) const;

    const std::map<Key, Image>& getImages() const { return mImages; }

private:
    std::map<Key, Image> mImages;
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
    const TranslatedVuProgram* find(u32 unit, u32 start, std::span<const u8> micro) const;
    VuMicro::Result runChecked(const TranslatedVuProgram& program, Vu& state, Memory& memory,
                               Gif* gif, u32 start);

    std::multimap<std::pair<u32, u32>, const TranslatedVuProgram*> mByStart;
    VuProgramLog* mLog;
    VuCapture* mCapture;
    bool mCheck;
};

}  // namespace uee
