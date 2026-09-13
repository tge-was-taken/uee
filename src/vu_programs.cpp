#include "vu_programs.h"

#include "log.h"
#include "memory.h"
#include "vu_capture.h"

#include <xxhash.h>

#include <algorithm>
#include <charconv>
#include <cstring>
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
    const auto mismatch =
        std::mismatch(expectedData.begin(), expectedData.end(), actualData.begin());
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

namespace {

std::vector<VuPairRange> getRangesOf(const std::vector<bool>& pairs) {
    std::vector<VuPairRange> ranges;
    for (u32 pc = 0; pc < pairs.size(); ++pc) {
        if (!pairs[pc]) {
            continue;
        }
        if (!ranges.empty() && ranges.back().first + ranges.back().count == pc) {
            ++ranges.back().count;
        } else {
            ranges.push_back({pc, 1});
        }
    }
    return ranges;
}

}  // namespace

bool VuProgramRecord::matches(std::span<const u8> micro) const {
    for (const VuPairRange& range : getRanges()) {
        const usize offset = range.first * kPairBytes;
        const usize bytes = range.count * kPairBytes;
        if (std::memcmp(code.data() + offset, micro.data() + offset, bytes) != 0) {
            return false;
        }
    }
    return true;
}

void VuProgramRecord::reach(u32 from, std::span<const u8> micro) {
    const std::vector<bool> before = reached;
    addReachablePairs(micro, from, jumps, reached);
    for (u32 pc = 0; pc < reached.size(); ++pc) {
        if (reached[pc] && !before[pc]) {
            std::copy_n(micro.data() + pc * kPairBytes, kPairBytes, code.data() + pc * kPairBytes);
        }
    }
}

// Running a pair the walk never reached means a jump the record did not know,
// so the pair joins the program from there.
void VuProgramRecord::cover(u32 pc, std::span<const u8> micro) {
    if (covered[pc]) {
        return;
    }
    covered[pc] = true;
    if (!reached[pc]) {
        reach(pc, micro);
    }
}

void VuProgramRecord::addJump(u32 site, u32 target, std::span<const u8> micro) {
    if (jumps[site].insert(target).second) {
        reach(site, micro);
    }
}

std::vector<VuPairRange> VuProgramRecord::getRanges() const {
    return getRangesOf(reached);
}

std::vector<VuPairRange> VuProgramRecord::getCoveredRanges() const {
    return getRangesOf(covered);
}

namespace {

std::unique_ptr<VuProgramRecord> makeRecord(u32 unit, u32 start) {
    const usize size = unit == 0 ? kVu0MemorySize : kVu1MemorySize;
    auto record = std::make_unique<VuProgramRecord>();
    record->unit = unit;
    record->start = start;
    record->code.assign(size, 0);
    record->reached.assign(size / kPairBytes, false);
    record->covered.assign(size / kPairBytes, false);
    return record;
}

constexpr std::string_view kVersionLine = "vu-programs 3";

}  // namespace

VuProgramRecord& VuProgramLog::find(u32 unit, u32 start, std::span<const u8> micro) {
    std::vector<std::unique_ptr<VuProgramRecord>>& records = mRecords[{unit, start}];
    for (auto it = records.begin(); it != records.end(); ++it) {
        if ((*it)->matches(micro)) {
            std::rotate(records.begin(), it, it + 1);
            return *records.front();
        }
    }
    records.insert(records.begin(), makeRecord(unit, start));
    records.front()->reach(start, micro);
    return *records.front();
}

std::vector<const VuProgramRecord*> VuProgramLog::getRecords() const {
    std::vector<const VuProgramRecord*> all;
    for (const auto& [key, records] : mRecords) {
        for (const auto& record : records) {
            all.push_back(record.get());
        }
    }
    return all;
}

// "vu-programs 3", then per record "program <unit> <start>", a "code <first>
// <hex>" line per run of reachable pairs, a "ran <first> <count>" line per run
// of pairs that ran and a "jump <site> <target>" per edge; pair indices in hex.
void VuProgramLog::load(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        return;
    }
    std::string line;
    if (!std::getline(file, line) || line != kVersionLine) {
        throw std::runtime_error(std::format(
            "{}: not a '{}' file, and an older one is not read",
            path.string(), kVersionLine));
    }
    VuProgramRecord* record = nullptr;
    while (std::getline(file, line)) {
        const usize space = line.find(' ');
        const std::string_view word = std::string_view(line).substr(0, space);
        const std::string_view rest =
            space == std::string::npos ? std::string_view{}
                                       : std::string_view(line).substr(space + 1);
        const usize split = rest.find(' ');
        const std::string_view first = rest.substr(0, split);
        const std::string_view second =
            split == std::string_view::npos ? std::string_view{} : rest.substr(split + 1);
        if (word == "program") {
            const u32 unit = parseNumber(first, 10, path);
            const u32 start = parseNumber(second, 16, path);
            auto& records = mRecords[{unit, start}];
            records.push_back(makeRecord(unit, start));
            record = records.back().get();
        } else if (record == nullptr) {
            throw std::runtime_error(
                std::format("{}: '{}' before any program", path.string(), word));
        } else if (word == "code") {
            const u32 pc = parseNumber(first, 16, path);
            const std::vector<u8> bytes = parseBytes(second, path);
            if (bytes.size() % kPairBytes != 0 ||
                pc * kPairBytes + bytes.size() > record->code.size()) {
                throw std::runtime_error(
                    std::format("{}: code at {:x} does not fit the unit", path.string(), pc));
            }
            std::fill_n(record->reached.begin() + pc, bytes.size() / kPairBytes, true);
            std::copy(bytes.begin(), bytes.end(), record->code.begin() + pc * kPairBytes);
        } else if (word == "ran") {
            const u32 pc = parseNumber(first, 16, path);
            const u32 count = parseNumber(second, 16, path);
            if (pc + count > record->covered.size()) {
                throw std::runtime_error(
                    std::format("{}: ran at {:x} does not fit the unit", path.string(), pc));
            }
            std::fill_n(record->covered.begin() + pc, count, true);
        } else if (word == "jump") {
            record->jumps[parseNumber(first, 16, path)].insert(parseNumber(second, 16, path));
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
    file << kVersionLine << '\n';
    for (const VuProgramRecord* record : getRecords()) {
        file << std::format("program {} {:x}\n", record->unit, record->start);
        for (const VuPairRange& range : record->getRanges()) {
            file << std::format("code {:x} ", range.first);
            for (usize i = 0; i < range.count * kPairBytes; ++i) {
                file << std::format("{:02x}", record->code[range.first * kPairBytes + i]);
            }
            file << '\n';
        }
        for (const VuPairRange& range : record->getCoveredRanges()) {
            file << std::format("ran {:x} {:x}\n", range.first, range.count);
        }
        for (const auto& [site, targets] : record->jumps) {
            for (const u32 target : targets) {
                file << std::format("jump {:x} {:x}\n", site, target);
            }
        }
    }
}

VuPrograms::VuPrograms(std::span<const TranslatedVuProgram> translated, VuProgramLog* log,
                       VuCapture* capture, bool check)
    : mLog(log), mCapture(capture), mCheck(check) {
    for (const TranslatedVuProgram& program : translated) {
        mByStart[{program.unit, program.start}].push_back(&program);
    }
}

VuMicro::Result VuPrograms::run(Vu& state, Memory& memory, Gif* gif, u32 unit, u32 start) {
    VuMicro micro(state, memory, unit, gif);
    if (mCapture == nullptr || !mCapture->isRecording()) {
        return runProgram(micro, start);
    }
    VuCapture::Run& run =
        mCapture->begin(unit, micro.instructions, state, micro.data, start,
                        memory.read32(getVifTopAddress(unit)),
                        memory.read32(getVifItopAddress(unit)));
    const VuMicro::Result result = runProgram(micro, start);
    mCapture->end(run, state, micro.data, result);
    return result;
}

VuMicro::Result VuPrograms::runProgram(VuMicro& micro, u32 start) {
    const std::span<const u8> code = micro.instructions;
    const u32 first = start % micro.getInstructionCount();
    micro.log = mLog;
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

const TranslatedVuProgram* VuPrograms::find(u32 unit, u32 start, std::span<const u8> micro) {
    const auto found = mByStart.find({unit, start});
    if (found == mByStart.end()) {
        return nullptr;
    }
    std::vector<const TranslatedVuProgram*>& candidates = found->second;
    for (auto it = candidates.begin(); it != candidates.end(); ++it) {
        const TranslatedVuProgram* program = *it;
        if (hashVuPairs(micro, program->pairs) == program->hash) {
            std::rotate(candidates.begin(), it, it + 1);
            return program;
        }
    }
    return nullptr;
}

}  // namespace uee
