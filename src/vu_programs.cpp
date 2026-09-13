#include "vu_programs.h"

#include "log.h"
#include "memory.h"
#include "vu_capture.h"

#include <xxhash.h>

#include <algorithm>
#include <charconv>
#include <format>
#include <fstream>
#include <stdexcept>
#include <string>

namespace uee {
namespace {

constexpr u32 kPairBytes = 8;

u32 parseNumber(std::string_view text, int base, const std::filesystem::path& path) {
    u32 value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, base);
    if (error != std::errc{} || end != text.data() + text.size()) {
        throw std::runtime_error(std::format("{}: '{}' is not a number", path.string(), text));
    }
    return value;
}

u64 parseHash(std::string_view text, const std::filesystem::path& path) {
    u64 value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    if (error != std::errc{} || end != text.data() + text.size()) {
        throw std::runtime_error(std::format("{}: '{}' is not a hash", path.string(), text));
    }
    return value;
}

std::vector<u8> parseBytes(std::string_view text, const std::filesystem::path& path) {
    if (text.size() % 2 != 0) {
        throw std::runtime_error(std::format("{}: odd number of hex digits", path.string()));
    }
    std::vector<u8> bytes(text.size() / 2);
    for (usize i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<u8>(parseNumber(text.substr(i * 2, 2), 16, path));
    }
    return bytes;
}

LoggerChannel log("vuprograms");

std::string describeDifference(const Vu& expected, const Vu& actual, VuMicro::Result expectedRun,
                               VuMicro::Result actualRun, std::span<const u8> expectedData,
                               std::span<const u8> actualData) {
    if (expectedRun.counter != actualRun.counter || expectedRun.ended != actualRun.ended) {
        return std::format("ended {} at {:x}, not {} at {:x}", actualRun.ended, actualRun.counter,
                           expectedRun.ended, expectedRun.counter);
    }
    for (u32 i = 0; i < 32; ++i) {
        if (expected.vf[i].lo != actual.vf[i].lo || expected.vf[i].hi != actual.vf[i].hi) {
            return std::format("vf{} {:016x}{:016x}, not {:016x}{:016x}", i, actual.vf[i].hi,
                               actual.vf[i].lo, expected.vf[i].hi, expected.vf[i].lo);
        }
        if (expected.vi[i] != actual.vi[i]) {
            return std::format("vi{} {:x}, not {:x}", i, actual.vi[i], expected.vi[i]);
        }
    }
    if (expected.acc.lo != actual.acc.lo || expected.acc.hi != actual.acc.hi) {
        return "the accumulator";
    }
    if (expected.clip != actual.clip) {
        return std::format("clip {:x}, not {:x}", actual.clip, expected.clip);
    }
    const auto mismatch = std::mismatch(expectedData.begin(), expectedData.end(), actualData.begin());
    if (mismatch.first != expectedData.end()) {
        return std::format("data memory at {:x}", mismatch.first - expectedData.begin());
    }
    return {};
}

}  // namespace

// Each range seeds the next, which needs no state object allocated per call.
u64 hashVuPairs(std::span<const u8> micro, std::span<const VuPairRange> pairs) {
    u64 hash = 0;
    for (const VuPairRange& range : pairs) {
        hash = XXH3_64bits_withSeed(micro.data() + range.first * kPairBytes,
                                    range.count * kPairBytes, hash);
    }
    return hash;
}

VuProgramLog::Image& VuProgramLog::recordStart(u32 unit, std::span<const u8> micro, u32 start) {
    const Key key{unit, XXH3_64bits(micro.data(), micro.size())};
    auto [found, added] = mImages.try_emplace(key);
    Image& image = found->second;
    if (added) {
        image.unit = unit;
        image.bytes.assign(micro.begin(), micro.end());
    }
    image.starts.insert(start);
    return image;
}

// One image per block: "image <unit> <hash>", "code <hex>", then its "start"
// and "jump" lines, pair indices in hex.
void VuProgramLog::load(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        return;
    }
    Image* image = nullptr;
    std::string line;
    while (std::getline(file, line)) {
        const usize space = line.find(' ');
        const std::string_view word = std::string_view(line).substr(0, space);
        const std::string_view rest =
            space == std::string::npos ? std::string_view{} : std::string_view(line).substr(space + 1);
        if (word == "image") {
            const usize split = rest.find(' ');
            const u32 unit = parseNumber(rest.substr(0, split), 10, path);
            const u64 hash = parseHash(rest.substr(split + 1), path);
            image = &mImages[{unit, hash}];
            image->unit = unit;
        } else if (image == nullptr) {
            throw std::runtime_error(std::format("{}: '{}' before any image", path.string(), word));
        } else if (word == "code") {
            image->bytes = parseBytes(rest, path);
        } else if (word == "start") {
            image->starts.insert(parseNumber(rest, 16, path));
        } else if (word == "jump") {
            image->jumps.insert(parseNumber(rest, 16, path));
        } else if (!word.empty()) {
            throw std::runtime_error(std::format("{}: unknown line '{}'", path.string(), word));
        }
    }
}

void VuProgramLog::save(const std::filesystem::path& path) const {
    std::ofstream file(path);
    if (!file) {
        throw std::runtime_error(std::format("cannot write {}", path.string()));
    }
    for (const auto& [key, image] : mImages) {
        file << std::format("image {} {:016x}\ncode ", key.first, key.second);
        for (const u8 byte : image.bytes) {
            file << std::format("{:02x}", byte);
        }
        file << '\n';
        for (const u32 start : image.starts) {
            file << std::format("start {:x}\n", start);
        }
        for (const u32 jump : image.jumps) {
            file << std::format("jump {:x}\n", jump);
        }
    }
}

VuPrograms::VuPrograms(std::span<const TranslatedVuProgram> translated, VuProgramLog* log,
                       VuCapture* capture, bool check)
    : mLog(log), mCapture(capture), mCheck(check) {
    for (const TranslatedVuProgram& program : translated) {
        mByStart.emplace(std::pair{program.unit, program.start}, &program);
    }
}

VuMicro::Result VuPrograms::run(Vu& state, Memory& memory, Gif* gif, u32 unit, u32 start) {
    VuMicro micro(state, memory, unit, gif);
    if (mCapture == nullptr || !mCapture->isRecording()) {
        return runProgram(micro, start);
    }
    VuCapture::Run& run =
        mCapture->begin(unit, micro.instructions, state, micro.data, start,
                        memory.read32(getVifTopAddress(unit)), memory.read32(getVifItopAddress(unit)));
    const VuMicro::Result result = runProgram(micro, start);
    mCapture->end(run, state, micro.data, result);
    return result;
}

VuMicro::Result VuPrograms::runProgram(VuMicro& micro, u32 start) {
    const std::span<const u8> code = micro.instructions;
    const u32 first = start % micro.getInstructionCount();
    if (mLog != nullptr) {
        micro.jumps = &mLog->recordStart(micro.unit, code, first).jumps;
    }
    const TranslatedVuProgram* program = find(micro.unit, first, code);
    if (program == nullptr) {
        return micro.run(start);
    }
    if (mCheck) {
        return runChecked(*program, micro.state, micro.memory, micro.gif, first);
    }
    micro.begin(first);
    return program->run(micro);
}

// The interpreter's run has the interface detached, so the kicks it makes do
// not reach the unit twice.
VuMicro::Result VuPrograms::runChecked(const TranslatedVuProgram& program, Vu& state,
                                       Memory& memory, Gif* gif, u32 start) {
    const Vu before = state;
    VuMicro reference(state, memory, program.unit, nullptr);
    const std::vector<u8> dataBefore(reference.data.begin(),
                                     reference.data.end());
    const VuMicro::Result expected = reference.run(start);
    const Vu expectedState = state;
    const std::vector<u8> expectedData(reference.data.begin(),
                                       reference.data.end());

    state = before;
    VuMicro micro(state, memory, program.unit, gif);
    std::copy(dataBefore.begin(), dataBefore.end(), micro.data.begin());
    micro.begin(start);
    const VuMicro::Result actual = program.run(micro);

    const std::string difference = describeDifference(expectedState, state, expected, actual,
                                                      expectedData, micro.data);
    if (!difference.empty()) {
        log.error("vu{} program at {:x} ({:016x}) differs from the interpreter: {}", program.unit,
                  program.start, program.hash, difference);
    }
    return actual;
}

const TranslatedVuProgram* VuPrograms::find(u32 unit, u32 start,
                                            std::span<const u8> micro) const {
    const auto [first, last] = mByStart.equal_range({unit, start});
    for (auto it = first; it != last; ++it) {
        if (hashVuPairs(micro, it->second->pairs) == it->second->hash) {
            return it->second;
        }
    }
    return nullptr;
}

}  // namespace uee
