// Replays micro program runs a game made, recorded with UEE_VU_CAPTURE, so VU
// work can be timed and checked without the game. Every run starts from the
// state and data memory it had in the game, and has to end where it did there.

#include "gif.h"
#include "gs.h"
#include "host_float.h"
#include "memory.h"
#include "vu_capture.h"
#include "vu_micro.h"
#include "vu_programs.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string_view>

#ifdef UEE_VU_PROGRAMS
namespace uee {
extern const std::span<const TranslatedVuProgram> translatedVuPrograms;
}
std::span<const uee::TranslatedVuProgram> getTranslatedVuPrograms() {
    return uee::translatedVuPrograms;
}
#else
std::span<const uee::TranslatedVuProgram> getTranslatedVuPrograms() {
    return {};
}
#endif

namespace {

using namespace uee;

// TPC is written after the capture closes a run, so the result stands for it.
bool isSameState(const Vu& a, const Vu& b) {
    for (s32 i = 0; i < 32; ++i) {
        if (a.vf[i].lo != b.vf[i].lo || a.vf[i].hi != b.vf[i].hi) {
            return false;
        }
        if (i != Vu::kTpc && a.vi[i] != b.vi[i]) {
            return false;
        }
    }
    return a.acc.lo == b.acc.lo && a.acc.hi == b.acc.hi && a.clip == b.clip;
}

struct Totals {
    std::chrono::nanoseconds elapsed{};
    u64 runs = 0;
    u64 mismatches = 0;
};

Totals replay(const VuCapture& capture, VuPrograms& programs, Memory& memory, Gif& gif,
              bool check) {
    Totals totals;
    Vu state;
    for (const VuCapture::Run& run : capture.getRuns()) {
        const std::span<const u8> image = capture.getImage(run.image);
        const u32 microBase = run.unit == 0 ? kVu0MicroBase : kVu1MicroBase;
        const u32 dataBase = run.unit == 0 ? kVu0DataBase : kVu1DataBase;
        const std::span<u8> micro = memory.getRegion(microBase, static_cast<u32>(image.size()));
        const std::span<u8> data =
            memory.getRegion(dataBase, static_cast<u32>(run.dataBefore.size()));
        std::copy(image.begin(), image.end(), micro.begin());
        std::copy(run.dataBefore.begin(), run.dataBefore.end(), data.begin());
        memory.write32(getVifTopAddress(run.unit), run.top);
        memory.write32(getVifItopAddress(run.unit), run.itop);
        state = run.before;
        state.programs = &programs;

        const auto begin = std::chrono::steady_clock::now();
        const bool ended = runMicroProgram(state, memory, run.unit == 1 ? &gif : nullptr,
                                           run.unit, run.start);
        totals.elapsed += std::chrono::steady_clock::now() - begin;
        ++totals.runs;

        if (!check) {
            continue;
        }
        const bool same = ended == run.result.ended &&
                          state.vi[Vu::kTpc] == run.result.counter &&
                          isSameState(state, run.after) && hashVuData(data) == run.dataAfter;
        if (!same && totals.mismatches++ < 5) {
            spdlog::error("run {} (vu{} at {:x}) ends differently from the game",
                          totals.runs - 1, run.unit, run.start);
        }
    }
    return totals;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        spdlog::error("usage: {} <capture> [--interpret] [--repeat <n>]", argv[0]);
        return 2;
    }
    bool interpret = false;
    u32 repeat = 5;
    for (int i = 2; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument == "--interpret") {
            interpret = true;
        } else if (argument == "--repeat" && i + 1 < argc) {
            repeat = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));
        } else {
            spdlog::error("unknown argument {}", argument);
            return 2;
        }
    }

    // The game gets this from its Interpreter; without it the host float path
    // rounds the wrong way.
    host::configureHostFloat();
    VuCapture capture;
    capture.load(argv[1]);
    Memory memory = makeMainMemory();
    Gs gs;
    Gif gif;
    gif.attach(memory, gs);
    VuPrograms programs{interpret ? std::span<const TranslatedVuProgram>{}
                                  : getTranslatedVuPrograms(),
                        nullptr, nullptr, false};

    // The first pass checks every result; the rest only time.
    std::chrono::nanoseconds best = std::chrono::nanoseconds::max();
    u64 mismatches = 0;
    for (u32 pass = 0; pass < std::max(repeat, 1u); ++pass) {
        const Totals totals = replay(capture, programs, memory, gif, pass == 0);
        mismatches += totals.mismatches;
        best = std::min(best, totals.elapsed);
    }
    const u64 runs = capture.getRuns().size();
    spdlog::info("{} runs, {}: best pass {:.2f} ms, {:.0f} ns per run, {} ended differently",
                 runs, interpret ? "interpreted" : "translated", best.count() / 1e6,
                 runs == 0 ? 0.0 : static_cast<double>(best.count()) / static_cast<double>(runs),
                 mismatches);
    return mismatches == 0 ? 0 : 1;
}
