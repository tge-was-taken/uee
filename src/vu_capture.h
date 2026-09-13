#pragma once

#include "types.h"
#include "vu.h"
#include "vu_micro.h"

#include <filesystem>
#include <map>
#include <span>
#include <vector>

namespace uee {

// Micro program runs kept whole, so a benchmark can replay the work a game gave
// the units without running the game, and check every result it gets.
class VuCapture {
public:
    struct Run {
        u32 unit = 0;
        u32 start = 0;
        u64 image = 0;
        // What the interface's TOP and ITOP read when the program started.
        u32 top = 0;
        u32 itop = 0;
        Vu before;
        std::vector<u8> dataBefore;
        Vu after;
        u64 dataAfter = 0;
        VuMicro::Result result{};
    };

    bool isRecording() const { return mRecording; }
    void setRecording(bool recording) { mRecording = recording; }

    Run& begin(u32 unit, std::span<const u8> micro, const Vu& state, std::span<const u8> data,
               u32 start, u32 top, u32 itop);
    void end(Run& run, const Vu& state, std::span<const u8> data, VuMicro::Result result);

    void save(const std::filesystem::path& path) const;
    void load(const std::filesystem::path& path);

    const std::vector<Run>& getRuns() const { return mRuns; }
    std::span<const u8> getImage(u64 hash) const { return mImages.at(hash); }

private:
    bool mRecording = false;
    std::vector<Run> mRuns;
    std::map<u64, std::vector<u8>> mImages;
};

u64 hashVuData(std::span<const u8> data);

}  // namespace uee
