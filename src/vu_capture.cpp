#include "vu_capture.h"

#include <xxhash.h>

#include <cstring>
#include <format>
#include <fstream>
#include <stdexcept>
#include <type_traits>

namespace uee {
namespace {

// The state is written as its bytes, so a capture only reads back into the
// build whose Vu has the same layout; the size in the header catches most
// changes to it.
static_assert(std::is_trivially_copyable_v<Vu>);

constexpr char kMagic[8] = {'U', 'E', 'E', 'V', 'U', 'R', 'U', 'N'};
constexpr u32 kVersion = 2;

template <typename T>
void writeValue(std::ofstream& file, const T& value) {
    file.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

void writeBytes(std::ofstream& file, std::span<const u8> bytes) {
    writeValue(file, static_cast<u32>(bytes.size()));
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

template <typename T>
T readValue(std::ifstream& file) {
    T value{};
    file.read(reinterpret_cast<char*>(&value), sizeof(value));
    return value;
}

std::vector<u8> readBytes(std::ifstream& file) {
    std::vector<u8> bytes(readValue<u32>(file));
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return bytes;
}

// The pointer to the program table means nothing in another process.
Vu withoutPrograms(const Vu& state) {
    Vu copy = state;
    copy.programs = nullptr;
    return copy;
}

}  // namespace

u64 hashVuData(std::span<const u8> data) {
    return XXH3_64bits(data.data(), data.size());
}

VuCapture::Run& VuCapture::begin(u32 unit, std::span<const u8> micro, const Vu& state,
                                 std::span<const u8> data, u32 start, u32 top, u32 itop) {
    const u64 image = XXH3_64bits(micro.data(), micro.size());
    mImages.try_emplace(image, micro.begin(), micro.end());
    Run& run = mRuns.emplace_back();
    run.unit = unit;
    run.start = start;
    run.image = image;
    run.top = top;
    run.itop = itop;
    run.before = withoutPrograms(state);
    run.dataBefore.assign(data.begin(), data.end());
    return run;
}

void VuCapture::end(Run& run, const Vu& state, std::span<const u8> data,
                    VuMicro::Result result) {
    run.after = withoutPrograms(state);
    run.dataAfter = hashVuData(data);
    run.result = result;
}

void VuCapture::save(const std::filesystem::path& path) const {
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error(std::format("cannot write {}", path.string()));
    }
    file.write(kMagic, sizeof(kMagic));
    writeValue(file, kVersion);
    writeValue(file, static_cast<u32>(sizeof(Vu)));
    writeValue(file, static_cast<u32>(mImages.size()));
    for (const auto& [hash, bytes] : mImages) {
        writeValue(file, hash);
        writeBytes(file, bytes);
    }
    writeValue(file, static_cast<u32>(mRuns.size()));
    for (const Run& run : mRuns) {
        writeValue(file, run.unit);
        writeValue(file, run.start);
        writeValue(file, run.image);
        writeValue(file, run.top);
        writeValue(file, run.itop);
        writeValue(file, run.before);
        writeBytes(file, run.dataBefore);
        writeValue(file, run.after);
        writeValue(file, run.dataAfter);
        writeValue(file, run.result.counter);
        writeValue(file, static_cast<u8>(run.result.ended ? 1 : 0));
    }
}

void VuCapture::load(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error(std::format("cannot read {}", path.string()));
    }
    char magic[sizeof(kMagic)] = {};
    file.read(magic, sizeof(magic));
    if (std::memcmp(magic, kMagic, sizeof(kMagic)) != 0 || readValue<u32>(file) != kVersion) {
        throw std::runtime_error(std::format("{} is not a VU run capture", path.string()));
    }
    const u32 stateSize = readValue<u32>(file);
    if (stateSize != sizeof(Vu)) {
        throw std::runtime_error(std::format(
            "{} holds {} byte unit states, this build's are {}", path.string(), stateSize,
            sizeof(Vu)));
    }
    const u32 images = readValue<u32>(file);
    for (u32 i = 0; i < images; ++i) {
        const u64 hash = readValue<u64>(file);
        mImages[hash] = readBytes(file);
    }
    const u32 runs = readValue<u32>(file);
    mRuns.reserve(runs);
    for (u32 i = 0; i < runs; ++i) {
        Run& run = mRuns.emplace_back();
        run.unit = readValue<u32>(file);
        run.start = readValue<u32>(file);
        run.image = readValue<u64>(file);
        run.top = readValue<u32>(file);
        run.itop = readValue<u32>(file);
        run.before = readValue<Vu>(file);
        run.dataBefore = readBytes(file);
        run.after = readValue<Vu>(file);
        run.dataAfter = readValue<u64>(file);
        run.result.counter = readValue<u32>(file);
        run.result.ended = readValue<u8>(file) != 0;
    }
    if (!file) {
        throw std::runtime_error(std::format("{} ends early", path.string()));
    }
}

}  // namespace uee
