#include "adpcm.h"
#include "spu2.h"
#include "vectors.h"

#include <spdlog/spdlog.h>

#include <array>
#include <format>
#include <string>
#include <vector>

namespace {

using namespace uee;
using namespace uee::test;

// Steps of PCSX2's SPU2: an ADPCM block with the two samples before it, and one
// sample's step of a voice envelope.

std::array<u8, kAdpcmBlockBytes> readBlock(const std::string& hex) {
    std::array<u8, kAdpcmBlockBytes> block{};
    for (usize i = 0; i < block.size(); ++i) {
        block[i] = static_cast<u8>(std::stoul(hex.substr(i * 2, 2), nullptr, 16));
    }
    return block;
}

std::string formatSamples(std::span<const s16> samples) {
    std::string text;
    for (const s16 sample : samples) {
        text += std::format("{} ", sample);
    }
    return text;
}

void compare(std::vector<Mismatch>& found, const std::string& field, s64 expected, s64 actual) {
    if (expected != actual) {
        found.push_back({field, std::to_string(expected), std::to_string(actual), ""});
    }
}

std::vector<Mismatch> checkAdpcm(const Json& row) {
    AdpcmState state{static_cast<s32>(row["prev1"].getSigned()),
                     static_cast<s32>(row["prev2"].getSigned())};
    std::array<s16, kAdpcmBlockSamples> samples{};
    decodeAdpcmBlock(readBlock(row["block"].getText()), state, samples);

    std::array<s16, kAdpcmBlockSamples> expected{};
    for (usize i = 0; i < expected.size(); ++i) {
        expected[i] = static_cast<s16>(row["samples"][i].getInteger());
    }
    std::vector<Mismatch> found;
    if (samples != expected) {
        found.push_back({"samples", formatSamples(expected), formatSamples(samples), ""});
    }
    compare(found, "prev", row["prev1Out"].getSigned(), state.previous);
    compare(found, "older", row["prev2Out"].getSigned(), state.older);
    return found;
}

// PCSX2 numbers the phases from stopped.
EnvelopePhase readPhase(const Json& value) {
    switch (value.getInteger()) {
        case 1: return EnvelopePhase::Attack;
        case 2: return EnvelopePhase::Decay;
        case 3: return EnvelopePhase::Sustain;
        case 4: return EnvelopePhase::Release;
        default: return EnvelopePhase::Off;
    }
}

Envelope readEnvelope(const Json& side) {
    return {readPhase(side["phase"]), static_cast<s32>(side["value"].getInteger()),
            static_cast<s32>(side["counter"].getInteger())};
}

std::vector<Mismatch> checkEnvelope(const Json& row) {
    const u32 registers = readHex(row["in"]["reg"]);
    Envelope envelope = readEnvelope(row["in"]);
    stepEnvelope(envelope, static_cast<u16>(registers), static_cast<u16>(registers >> 16));
    const Envelope expected = readEnvelope(row["out"]);
    std::vector<Mismatch> found;
    compare(found, "phase", static_cast<s64>(expected.phase), static_cast<s64>(envelope.phase));
    compare(found, "level", expected.level, envelope.level);
    compare(found, "counter", expected.counter, envelope.counter);
    return found;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        spdlog::error("usage: test_spu_vectors <directory holding spu_*_vectors.jsonl> [examples]");
        return 2;
    }
    const std::string directory = argv[1];
    const usize examples = argc > 2 ? std::stoul(argv[2]) : 2;

    Report adpcm(examples);
    readRecords(directory + "/spu_adpcm_vectors.jsonl", [&](const Json& row) {
        adpcm.addCase("adpcm " + row["form"].getText(),
                      std::format("block {} prev {} {}", row["block"].getText(),
                                  row["prev1"].getSigned(), row["prev2"].getSigned()),
                      checkAdpcm(row));
    });
    Report envelopes(examples);
    readRecords(directory + "/spu_adsr_vectors.jsonl", [&](const Json& row) {
        const Json& in = row["in"];
        envelopes.addCase("adsr " + row["form"].getText(),
                          std::format("reg {} phase {} level {} counter {}", in["reg"].getText(),
                                      in["phase"].getInteger(), in["value"].getInteger(),
                                      in["counter"].getInteger()),
                          checkEnvelope(row));
    });
    const int adpcmResult = adpcm.finish("adpcm blocks");
    const int envelopeResult = envelopes.finish("envelope steps");
    return adpcmResult | envelopeResult;
}
