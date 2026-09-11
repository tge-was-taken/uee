#include "json.h"
#include "memory.h"
#include "vector_memory.h"
#include "vu.h"
#include "vu_micro.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <format>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace uee;
using test::Json;

// One VU1 instruction pair as PCSX2's interpreter stepped it, from
// pcsx2-hook's PCSX2HOOK_VU1_INSN_TRACE. The interpreter writes VF, VI and ACC
// at once and delays only Q, P and the flags, so those three come out of the
// pipeline state the record carries rather than the register file.

constexpr u32 kNopUpper = 0x000002FF;
constexpr u32 kNopLower = 0x8000033C;
constexpr u32 kInstructions = kVu1MemorySize / 8;
// Long enough for the slowest elementary function to land.
constexpr u32 kSettleSteps = 64;

constexpr u32 kClipFlagBit = 1u << Vu::kClip;
constexpr u32 kStatusFlagBit = 1u << Vu::kStatus;

const std::set<std::string> kBranchForms = {"B",    "BAL",  "JR",    "JALR",  "IBEQ",
                                            "IBNE", "IBLTZ", "IBGTZ", "IBLEZ", "IBGEZ"};
const std::set<std::string> kQuotientForms = {"DIV", "SQRT", "RSQRT"};
const std::set<std::string> kFunctionForms = {"ESADD", "ERSADD", "ELENG", "ERLENG", "EATANxy",
                                              "EATANxz", "ESUM", "ESQRT", "ERSQRT", "ERCPR",
                                              "ESIN", "EATAN", "EEXP"};

Reg128 toReg128(const Json& hex) {
    const auto [lo, hi] = hex.getHex128();
    return Reg128{lo, hi};
}

std::string hex128(const Reg128& r) {
    return std::format("{:08X}{:08X}{:08X}{:08X}", r.word(3), r.word(2), r.word(1), r.word(0));
}

double magnitude(u32 bits) {
    return std::fabs(static_cast<double>(std::bit_cast<float>(bits & 0x7FFFFFFFu)));
}

// The interpreter rounds on the host, twice where an accumulate is involved,
// while this runtime follows the console's single rounding. Where the operands
// nearly cancel, a few places at their scale is most of the result, so the
// allowance is measured against the largest value the lane went through.
constexpr double kRoundingAllowance = 64.0 / (1 << 23);

bool isRounding(u32 expected, u32 actual, double scale) {
    const double difference = std::fabs(static_cast<double>(std::bit_cast<float>(expected)) -
                                        static_cast<double>(std::bit_cast<float>(actual)));
    scale = std::max({scale, magnitude(expected), magnitude(actual)});
    return difference <= scale * kRoundingAllowance;
}

// What the program could read when the pair ran: the interpreter retires every
// pipeline slot old enough by then, after any stall the pair took.
struct Readable {
    u32 status;
    u32 mac;
    u32 clip;
    u32 q;
    u32 p;
};

Readable getReadable(const Json& row) {
    const Json& vi = row["in"]["vi"];
    const Json& pipe = row["pipeIn"];
    const u64 cycles = row["cycles"].getInteger();
    Readable r{static_cast<u32>(vi[Vu::kStatus].getHex()), static_cast<u32>(vi[Vu::kMac].getHex()),
               static_cast<u32>(vi[Vu::kClip].getHex()), static_cast<u32>(vi[Vu::kQ].getHex()),
               static_cast<u32>(vi[Vu::kP].getHex())};

    for (const Json& slot : pipe["fmac"].getItems()) {
        if (slot["age"].getInteger() + cycles < slot["latency"].getInteger()) {
            break;
        }
        const u32 flagreg = static_cast<u32>(slot["flagreg"].getSigned());
        const u32 status = static_cast<u32>(slot["status"].getHex());
        if ((flagreg & kClipFlagBit) != 0) {
            r.clip = static_cast<u32>(slot["clip"].getHex());
        }
        if ((flagreg & kStatusFlagBit) != 0) {
            r.status = (r.status & 0x30u) | (status & 0xFC0u) | (status & 0xFu);
        } else {
            r.status = (r.status & 0xFF0u) | (status & 0xFu) | ((status & 0xFu) << 6);
        }
        r.mac = static_cast<u32>(slot["mac"].getHex());
    }
    const Json& fdiv = pipe["fdiv"];
    if (fdiv["enable"].getInteger() != 0 &&
        fdiv["age"].getInteger() + cycles >= fdiv["latency"].getInteger()) {
        r.q = static_cast<u32>(fdiv["reg"].getHex());
        r.status = (r.status & 0xFCFu) | (static_cast<u32>(fdiv["status"].getHex()) & 0xC30u);
    }
    const Json& efu = pipe["efu"];
    if (efu["enable"].getInteger() != 0 &&
        efu["age"].getInteger() + cycles >= efu["latency"].getInteger()) {
        r.p = static_cast<u32>(efu["reg"].getHex());
    }
    return r;
}

void seed(Vu& vu, Memory& memory, const Json& row) {
    const Json& in = row["in"];
    const Json& pipe = row["pipeIn"];
    for (u32 i = 0; i < 32; ++i) {
        vu.vf[i] = toReg128(in["vf"][i]);
        vu.vi[i] = static_cast<u32>(in["vi"][i].getHex());
    }
    vu.acc = toReg128(in["acc"]);
    std::fill(std::begin(vu.accSaturated), std::end(vu.accSaturated), false);

    const Readable readable = getReadable(row);
    vu.mSeenFlags = {readable.status, readable.mac};
    vu.mStagedLive = vu.mSeenFlags;
    vu.mStagedFull = vu.mSeenFlags;
    vu.vi[Vu::kStatus] = (readable.status & ~0xFu) | (static_cast<u32>(pipe["status"].getHex()) & 0xFu);
    vu.vi[Vu::kMac] = static_cast<u32>(pipe["mac"].getHex());
    vu.vi[Vu::kClip] = readable.clip;
    vu.clip = static_cast<u32>(pipe["clip"].getHex());
    vu.vi[Vu::kQ] = readable.q;
    vu.vi[Vu::kP] = readable.p;

    for (const Json& quad : row["mem"].getItems()) {
        const u32 address = kVu1DataBase + static_cast<u32>(quad["addr"].getHex()) * 16;
        test::writeQuadword(memory, address, quad["before"].getText());
    }

    const u32 pc = static_cast<u32>(row["pc"].getHex());
    memory.write32(kVu1MicroBase + pc * 8, static_cast<u32>(row["lower"].getHex()));
    memory.write32(kVu1MicroBase + pc * 8 + 4, static_cast<u32>(row["upper"].getHex()));
    for (u32 i = 1; i <= kSettleSteps + 1; ++i) {
        const u32 at = kVu1MicroBase + ((pc + i) % kInstructions) * 8;
        memory.write32(at, kNopLower);
        memory.write32(at + 4, kNopUpper);
    }
}

// A mismatch with a reason is a known disagreement between the interpreter and
// the console; one without is a failure.
struct Mismatch {
    std::string field;
    std::string expected;
    std::string actual;
    std::string reason;
};

// The largest value each lane of the upper instruction could have gone through:
// its operands, their product and the accumulator it adds to.
std::array<double, 4> getLaneScales(const Vu& before, u32 upper) {
    const Reg128 fs = before.readVf((upper >> 11) & 0x1Fu);
    const Reg128 ft = before.readVf((upper >> 16) & 0x1Fu);
    double largestFt = std::max(magnitude(before.vi[Vu::kQ]), magnitude(before.vi[Vu::kI]));
    for (s32 i = 0; i < 4; ++i) {
        largestFt = std::max(largestFt, magnitude(ft.word(i)));
    }
    std::array<double, 4> scales{};
    for (s32 i = 0; i < 4; ++i) {
        scales[i] = std::max({magnitude(before.acc.word(i)), magnitude(fs.word(i)), largestFt,
                              magnitude(fs.word(i)) * largestFt});
    }
    return scales;
}

// PCSX2 clamps to the host's largest float, where the unit has no infinity and
// carries on through the top exponent.
bool isHostLargest(u32 expected, u32 actual) {
    return (expected & 0x7FFFFFFFu) == 0x7F7FFFFFu && (actual & 0x7F800000u) == 0x7F800000u &&
           ((expected ^ actual) & 0x80000000u) == 0;
}

// The outer product leaves w alone, and PCSX2 leaves that lane's flags from
// whatever ran before rather than clearing them.
constexpr u32 kMacLaneW = 0x1111;
const std::set<std::string> kOuterProductForms = {"OPMULA", "OPMSUB"};

// The integer file of the interface these read is not part of the record.
const std::set<std::string> kInterfaceForms = {"XTOP", "XITOP"};

std::vector<Mismatch> check(Vu& vu, Memory& memory, const Json& row) {
    std::vector<Mismatch> found;
    auto compare = [&](const std::string& field, u64 expected, u64 actual, std::string reason = "") {
        if (expected != actual) {
            found.push_back({field, std::format("{:08X}", expected), std::format("{:08X}", actual),
                             std::move(reason)});
        }
    };
    const std::array<double, 4> scales =
        getLaneScales(vu, static_cast<u32>(row["upper"].getHex()));
    const std::string& lform = row["lform"].getText();

    const Json& in = row["in"];
    Reg128 vf[32];
    u32 vi[32];
    Reg128 acc = toReg128(in["acc"]);
    for (u32 i = 0; i < 32; ++i) {
        vf[i] = toReg128(in["vf"][i]);
        vi[i] = static_cast<u32>(in["vi"][i].getHex());
    }
    for (const Json& delta : row["deltas"].getItems()) {
        const std::string& file = delta["file"].getText();
        const u32 n = static_cast<u32>(delta["n"].getInteger());
        if (file == "vf") {
            vf[n] = toReg128(delta["after"]);
        } else if (file == "vi") {
            vi[n] = static_cast<u32>(delta["after"].getHex());
        } else {
            acc = toReg128(delta["after"]);
        }
    }

    const u32 pc = static_cast<u32>(row["pc"].getHex());
    VuMicro micro(vu, memory, 1, nullptr);
    micro.begin(pc);
    micro.step();

    std::string flagReason;
    auto compare128 = [&](const std::string& field, const Reg128& expected, const Reg128& actual) {
        if (expected.lo == actual.lo && expected.hi == actual.hi) {
            return;
        }
        bool rounding = true;
        bool clamped = false;
        for (s32 i = 0; i < 4; ++i) {
            const u32 e = expected.word(i), a = actual.word(i);
            if (isHostLargest(e, a)) {
                clamped = true;
            } else {
                rounding = rounding && isRounding(e, a, scales[i]);
            }
        }
        const std::string reason = !rounding ? "" : clamped ? "host largest" : "rounding";
        if (!reason.empty()) {
            flagReason = reason;
        }
        found.push_back({field, hex128(expected), hex128(actual), reason});
    };
    for (u32 i = 1; i < 32; ++i) {
        compare128(std::format("vf{}", i), vf[i], vu.vf[i]);
    }
    compare128("acc", acc, vu.acc);
    const std::string interfaceReason = kInterfaceForms.contains(lform) ? "interface" : "";
    for (u32 i = 1; i < 16; ++i) {
        compare(std::format("vi{}", i), vi[i] & 0xFFFFu, vu.vi[i] & 0xFFFFu, interfaceReason);
    }
    compare("I", vi[Vu::kI], vu.vi[Vu::kI]);
    compare("R", vi[Vu::kR] & 0x7FFFFFu, vu.vi[Vu::kR] & 0x7FFFFFu);

    const Json& out = row["pipeOut"];
    const u32 expectedMac = static_cast<u32>(out["mac"].getHex());
    if (flagReason.empty() && kOuterProductForms.contains(row["uform"].getText()) &&
        ((expectedMac ^ vu.vi[Vu::kMac]) & ~kMacLaneW) == 0) {
        flagReason = "stale w flags";
    }
    compare("mac", expectedMac, vu.vi[Vu::kMac], flagReason);
    compare("status", out["status"].getHex() & 0xFu, vu.vi[Vu::kStatus] & 0xFu, flagReason);
    compare("clip", out["clip"].getHex(), vu.clip);

    for (const Json& quad : row["mem"].getItems()) {
        const u32 address = kVu1DataBase + static_cast<u32>(quad["addr"].getHex()) * 16;
        const std::string actual = test::readQuadword(memory, address);
        if (actual != quad["after"].getText()) {
            found.push_back({"mem" + quad["addr"].getText(), quad["after"].getText(), actual, ""});
        }
    }

    const bool delaySlot = row["pipeIn"]["branch"]["count"].getInteger() != 0;
    if (kBranchForms.contains(lform) && !delaySlot) {
        micro.step();
        const Json& branch = out["branch"];
        const u32 expected = branch["count"].getInteger() != 0
                                 ? static_cast<u32>(branch["pc"].getHex())
                                 : (pc + 2) % kInstructions;
        compare("branch", expected, micro.getProgramCounter() % kInstructions);
    }

    const Json& fdiv = out["fdiv"];
    if (kQuotientForms.contains(lform) && fdiv["enable"].getInteger() != 0 &&
        fdiv["age"].getInteger() == 0) {
        compare("divide flags", fdiv["status"].getHex() & 0x30u, vu.vi[Vu::kStatus] & 0x30u);
        for (u32 i = 0; i < kSettleSteps; ++i) {
            micro.step();
        }
        const u32 q = static_cast<u32>(fdiv["reg"].getHex());
        const u32 actual = vu.vi[Vu::kQ];
        compare("Q", q, actual,
                isHostLargest(q, actual) ? "host largest"
                : isRounding(q, actual, 0.0) ? "rounding" : "");
    }
    const Json& efu = out["efu"];
    if (kFunctionForms.contains(lform) && efu["enable"].getInteger() != 0 &&
        efu["age"].getInteger() == 0) {
        for (u32 i = 0; i < kSettleSteps; ++i) {
            micro.step();
        }
        const u32 p = static_cast<u32>(efu["reg"].getHex());
        compare("P", p, vu.vi[Vu::kP], isRounding(p, vu.vi[Vu::kP], 0.0) ? "rounding" : "");
    }
    return found;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        spdlog::error("usage: test_vu1_vectors <vu1_vectors.jsonl> [examples per form]");
        return 2;
    }
    const usize examples = argc > 2 ? std::stoul(argv[2]) : 2;
    std::ifstream file(argv[1]);
    if (!file) {
        spdlog::error("cannot open {}", argv[1]);
        return 2;
    }

    Memory memory = makeMainMemory();
    std::map<std::string, usize> failuresByKey;
    std::map<std::string, usize> knownByReason;
    std::map<std::string, std::vector<std::string>> examplesByKey;
    usize cases = 0, failed = 0;

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) {
            continue;
        }
        const Json row = Json::parse(line);
        Vu vu;
        seed(vu, memory, row);
        const std::vector<Mismatch> found = check(vu, memory, row);
        ++cases;

        std::set<std::string> fields;
        std::set<std::string> reasons;
        for (const Mismatch& m : found) {
            if (!m.reason.empty()) {
                reasons.insert(m.reason);
                continue;
            }
            std::string category;
            for (const char c : m.field.starts_with("mem") ? std::string("mem") : m.field) {
                if (c < '0' || c > '9') {
                    category += c;
                }
            }
            fields.insert(category);
        }
        for (const std::string& reason : reasons) {
            ++knownByReason[reason];
        }
        if (fields.empty()) {
            continue;
        }
        ++failed;
        const std::string pair = row["uform"].getText() + "/" + row["lform"].getText();
        for (const std::string& field : fields) {
            const std::string key = pair + " " + field;
            ++failuresByKey[key];
            std::vector<std::string>& list = examplesByKey[key];
            if (list.size() < examples) {
                std::string text = std::format("pc {} {} {} ({} | {})", row["pc"].getText(),
                                               row["upper"].getText(), row["lower"].getText(),
                                               row["umnem"].getText(), row["lmnem"].getText());
                for (const Mismatch& m : found) {
                    text += std::format("\n      {}: expected {} got {}{}", m.field, m.expected,
                                        m.actual, m.reason.empty() ? "" : " (" + m.reason + ")");
                }
                list.push_back(text);
            }
        }
    }

    std::vector<std::pair<std::string, usize>> sorted(failuresByKey.begin(), failuresByKey.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    for (const auto& [key, count] : sorted) {
        std::printf("%5zu  %s\n", count, key.c_str());
        for (const std::string& text : examplesByKey[key]) {
            std::printf("    %s\n", text.c_str());
        }
    }
    for (const auto& [reason, count] : knownByReason) {
        std::printf("known: %zu cases differ by %s\n", count, reason.c_str());
    }
    std::printf("%zu of %zu cases fail\n", failed, cases);
    return failed == 0 ? 0 : 1;
}
