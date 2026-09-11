#include "adpcm.h"
#include "json.h"
#include "spu2.h"

#include <spdlog/spdlog.h>

#include <array>
#include <format>
#include <fstream>
#include <map>
#include <string>

namespace {

using namespace uee;
using test::Json;

// Steps of PCSX2's SPU2, from pcsx2-hook's PCSX2HOOK_SPU_TRACE: an ADPCM block
// with the two samples before it, and one sample's step of a voice envelope.

struct Result {
    usize cases = 0;
    usize failed = 0;
};

std::array<u8, kAdpcmBlockBytes> readBlock(const std::string& hex) {
    std::array<u8, kAdpcmBlockBytes> block{};
    for (usize i = 0; i < block.size(); ++i) {
        block[i] = static_cast<u8>(std::stoul(hex.substr(i * 2, 2), nullptr, 16));
    }
    return block;
}

std::string describe(std::span<const s16> samples) {
    std::string text;
    for (const s16 sample : samples) {
        text += std::format(" {}", sample);
    }
    return text;
}

Result checkAdpcm(const std::string& path, usize examples) {
    std::ifstream file(path);
    if (!file) {
        spdlog::error("cannot open {}", path);
        std::exit(2);
    }
    Result result;
    std::map<std::string, usize> failuresByForm;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) {
            continue;
        }
        const Json row = Json::parse(line);
        const std::array<u8, kAdpcmBlockBytes> block = readBlock(row["block"].getText());
        AdpcmState state{static_cast<s32>(row["prev1"].getSigned()),
                         static_cast<s32>(row["prev2"].getSigned())};
        std::array<s16, kAdpcmBlockSamples> samples{};
        decodeAdpcmBlock(block, state, samples);

        std::array<s16, kAdpcmBlockSamples> expected{};
        const Json& recorded = row["samples"];
        for (usize i = 0; i < expected.size(); ++i) {
            expected[i] = static_cast<s16>(recorded[i].getInteger());
        }
        ++result.cases;
        const bool same = samples == expected &&
                          state.previous == row["prev1Out"].getSigned() &&
                          state.older == row["prev2Out"].getSigned();
        if (same) {
            continue;
        }
        ++result.failed;
        const std::string& form = row["form"].getText();
        if (failuresByForm[form]++ < examples) {
            std::printf("adpcm %s block %s prev %lld %lld\n  expected%s\n  got     %s\n",
                        form.c_str(), row["block"].getText().c_str(),
                        static_cast<long long>(row["prev1"].getSigned()),
                        static_cast<long long>(row["prev2"].getSigned()),
                        describe(expected).c_str(), describe(samples).c_str());
        }
    }
    for (const auto& [form, count] : failuresByForm) {
        std::printf("%5zu  adpcm %s\n", count, form.c_str());
    }
    std::printf("adpcm: %zu of %zu cases fail\n", result.failed, result.cases);
    return result;
}

// PCSX2 numbers the phases from stopped.
EnvelopePhase toPhase(u64 recorded) {
    switch (recorded) {
        case 1: return EnvelopePhase::Attack;
        case 2: return EnvelopePhase::Decay;
        case 3: return EnvelopePhase::Sustain;
        case 4: return EnvelopePhase::Release;
        default: return EnvelopePhase::Off;
    }
}

Envelope readEnvelope(const Json& side) {
    return {toPhase(side["phase"].getInteger()), static_cast<s32>(side["value"].getInteger()),
            static_cast<s32>(side["counter"].getInteger())};
}

Result checkEnvelopes(const std::string& path, usize examples) {
    std::ifstream file(path);
    if (!file) {
        spdlog::error("cannot open {}", path);
        std::exit(2);
    }
    Result result;
    std::map<std::string, usize> failuresByForm;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) {
            continue;
        }
        const Json row = Json::parse(line);
        const u32 registers = static_cast<u32>(row["in"]["reg"].getHex());
        Envelope envelope = readEnvelope(row["in"]);
        stepEnvelope(envelope, static_cast<u16>(registers), static_cast<u16>(registers >> 16));
        const Envelope expected = readEnvelope(row["out"]);
        ++result.cases;
        if (envelope.phase == expected.phase && envelope.level == expected.level &&
            envelope.counter == expected.counter) {
            continue;
        }
        ++result.failed;
        const std::string& form = row["form"].getText();
        if (failuresByForm[form]++ < examples) {
            std::printf("adsr %s reg %08x in phase %d level %d counter %d\n"
                        "  expected phase %d level %d counter %d\n"
                        "  got      phase %d level %d counter %d\n",
                        form.c_str(), registers, static_cast<int>(toPhase(row["in"]["phase"].getInteger())),
                        static_cast<int>(row["in"]["value"].getInteger()),
                        static_cast<int>(row["in"]["counter"].getInteger()),
                        static_cast<int>(expected.phase), expected.level, expected.counter,
                        static_cast<int>(envelope.phase), envelope.level, envelope.counter);
        }
    }
    for (const auto& [form, count] : failuresByForm) {
        std::printf("%5zu  adsr %s\n", count, form.c_str());
    }
    std::printf("adsr: %zu of %zu cases fail\n", result.failed, result.cases);
    return result;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        spdlog::error("usage: test_spu_vectors <directory holding spu_*_vectors.jsonl> [examples]");
        return 2;
    }
    const std::string directory = argv[1];
    const usize examples = argc > 2 ? std::stoul(argv[2]) : 2;
    const Result adpcm = checkAdpcm(directory + "/spu_adpcm_vectors.jsonl", examples);
    const Result envelopes = checkEnvelopes(directory + "/spu_adsr_vectors.jsonl", examples);
    return adpcm.failed == 0 && envelopes.failed == 0 ? 0 : 1;
}
