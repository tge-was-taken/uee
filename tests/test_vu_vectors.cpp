#include "vectors.h"
#include "vu.h"
#include "vu_micro.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <format>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace uee;
using namespace uee::test;

// Each record is one micro mode pair as PCSX2's interpreter stepped it. Only Q, P
// and the flags are delayed there, so those come from the pipeline state.

constexpr u32 kNopUpper = 0x000002FF;
constexpr u32 kNopLower = 0x8000033C;
// Long enough for the slowest elementary function to land.
constexpr u32 kLandingSteps = 64;

const std::set<std::string> kBranchForms = {"B",    "BAL",  "JR",    "JALR",  "IBEQ",
                                            "IBNE", "IBLTZ", "IBGTZ", "IBLEZ", "IBGEZ"};
const std::set<std::string> kQuotientForms = {"DIV", "SQRT", "RSQRT"};
const std::set<std::string> kFunctionForms = {"ESADD", "ERSADD", "ELENG", "ERLENG", "EATANxy",
                                              "EATANxz", "ESUM", "ESQRT", "ERSQRT", "ERCPR",
                                              "ESIN", "EATAN", "EEXP"};
const std::set<std::string> kOuterProductForms = {"OPMULA", "OPMSUB"};
// The integer file of the interface these read is not part of the record.
const std::set<std::string> kInterfaceForms = {"XTOP", "XITOP"};

// The first unit reaches the second one's registers through the top of its
// data window, which the recorder marks and this runtime does not model.
bool reachesOtherUnit(const Json& row) {
    const std::vector<Json>& quads = row["mem"].getItems();
    return std::any_of(quads.begin(), quads.end(),
                       [](const Json& quad) { return quad.find("space") != nullptr; });
}

double getMagnitude(u32 bits) {
    return std::fabs(static_cast<double>(std::bit_cast<float>(bits & 0x7FFFFFFFu)));
}

// PCSX2 rounds on the host, twice for an accumulate. Where the operands nearly
// cancel, a few places at their scale is most of the result.
constexpr double kRoundingAllowance = 64.0 / (1 << 23);

bool isRounding(u32 expected, u32 actual, double scale) {
    const double difference = std::fabs(static_cast<double>(std::bit_cast<float>(expected)) -
                                        static_cast<double>(std::bit_cast<float>(actual)));
    scale = std::max({scale, getMagnitude(expected), getMagnitude(actual)});
    return difference <= scale * kRoundingAllowance;
}

// PCSX2 clamps to the host's largest float, where the unit has no infinity and
// carries on through the top exponent.
bool isHostLargest(u32 expected, u32 actual) {
    return (expected & 0x7FFFFFFFu) == 0x7F7FFFFFu && (actual & 0x7F800000u) == 0x7F800000u &&
           ((expected ^ actual) & 0x80000000u) == 0;
}

// The largest value each lane of the upper instruction could have gone through:
// its operands, their product and the accumulator it adds to.
std::array<double, 4> getLaneScales(const Vu& before, u32 upper) {
    const Reg128 fs = before.readVf((upper >> 11) & 0x1Fu);
    const Reg128 ft = before.readVf((upper >> 16) & 0x1Fu);
    double largestFt = std::max(getMagnitude(before.vi[Vu::kQ]), getMagnitude(before.vi[Vu::kI]));
    for (s32 i = 0; i < 4; ++i) {
        largestFt = std::max(largestFt, getMagnitude(ft.word(i)));
    }
    std::array<double, 4> scales{};
    for (s32 i = 0; i < 4; ++i) {
        scales[i] = std::max({getMagnitude(before.acc.word(i)), getMagnitude(fs.word(i)),
                              largestFt, getMagnitude(fs.word(i)) * largestFt});
    }
    return scales;
}

// Empty when the lanes differ by more than rounding or PCSX2's clamp.
std::string getVectorReason(const Reg128& expected, const Reg128& actual,
                            const std::array<double, 4>& scales) {
    bool clamped = false;
    for (s32 i = 0; i < 4; ++i) {
        if (isHostLargest(expected.word(i), actual.word(i))) {
            clamped = true;
        } else if (!isRounding(expected.word(i), actual.word(i), scales[i])) {
            return "";
        }
    }
    return clamped ? "host largest" : "rounding";
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
    auto hasLanded = [cycles](const Json& slot) {
        return slot["age"].getInteger() + cycles >= slot["latency"].getInteger();
    };
    Readable r{readHex(vi[Vu::kStatus]), readHex(vi[Vu::kMac]), readHex(vi[Vu::kClip]),
               readHex(vi[Vu::kQ]), readHex(vi[Vu::kP])};

    for (const Json& slot : pipe["fmac"].getItems()) {
        if (!hasLanded(slot)) {
            break;
        }
        const u32 written = static_cast<u32>(slot["flagreg"].getSigned());
        const u32 status = readHex(slot["status"]);
        if ((written & (1u << Vu::kClip)) != 0) {
            r.clip = readHex(slot["clip"]);
        }
        if ((written & (1u << Vu::kStatus)) != 0) {
            r.status = (r.status & 0x30u) | (status & 0xFC0u) | (status & 0xFu);
        } else {
            r.status = (r.status & 0xFF0u) | (status & 0xFu) | ((status & 0xFu) << 6);
        }
        r.mac = readHex(slot["mac"]);
    }
    const Json& fdiv = pipe["fdiv"];
    if (fdiv["enable"].getInteger() != 0 && hasLanded(fdiv)) {
        r.q = readHex(fdiv["reg"]);
        r.status = (r.status & 0xFCFu) | (readHex(fdiv["status"]) & 0xC30u);
    }
    const Json& efu = pipe["efu"];
    if (efu["enable"].getInteger() != 0 && hasLanded(efu)) {
        r.p = readHex(efu["reg"]);
    }
    return r;
}

void seed(Vu& vu, Memory& memory, const VuUnit& unit, const Json& row) {
    const Json& in = row["in"];
    const Json& pipe = row["pipeIn"];
    for (u32 i = 0; i < 32; ++i) {
        vu.vf[i] = readReg128(in["vf"][i]);
        vu.vi[i] = readHex(in["vi"][i]);
    }
    vu.loadAcc(readReg128(in["acc"]));

    const Readable readable = getReadable(row);
    vu.seenFlags = {readable.status, readable.mac};
    vu.vi[Vu::kStatus] = (readable.status & ~0xFu) | (readHex(pipe["status"]) & 0xFu);
    vu.vi[Vu::kMac] = readHex(pipe["mac"]);
    vu.vi[Vu::kClip] = readable.clip;
    vu.clip = readHex(pipe["clip"]);
    vu.vi[Vu::kQ] = readable.q;
    vu.vi[Vu::kP] = readable.p;

    for (const Json& quad : row["mem"].getItems()) {
        writeQuadword(memory, unit.data + readHex(quad["addr"]) * 16, quad["before"].getText());
    }
    const u32 pc = readHex(row["pc"]);
    memory.write32(unit.micro + pc * 8, readHex(row["lower"]));
    memory.write32(unit.micro + pc * 8 + 4, readHex(row["upper"]));
    for (u32 i = 1; i <= kLandingSteps + 1; ++i) {
        const u32 at = unit.micro + ((pc + i) % unit.getInstructionCount()) * 8;
        memory.write32(at, kNopLower);
        memory.write32(at + 4, kNopUpper);
    }
}

struct Expected {
    Reg128 vf[32];
    u32 vi[32];
    Reg128 acc;
};

Expected readExpected(const Json& row) {
    const Json& in = row["in"];
    Expected expected{};
    for (u32 i = 0; i < 32; ++i) {
        expected.vf[i] = readReg128(in["vf"][i]);
        expected.vi[i] = readHex(in["vi"][i]);
    }
    expected.acc = readReg128(in["acc"]);
    for (const Json& delta : row["deltas"].getItems()) {
        const std::string& file = delta["file"].getText();
        const u32 n = readInteger(delta["n"]);
        if (file == "vf") {
            expected.vf[n] = readReg128(delta["after"]);
        } else if (file == "vi") {
            expected.vi[n] = readHex(delta["after"]);
        } else {
            expected.acc = readReg128(delta["after"]);
        }
    }
    return expected;
}

std::vector<Mismatch> check(Vu& vu, Memory& memory, const VuUnit& unit, const Json& row) {
    std::vector<Mismatch> found;
    auto compare = [&](const std::string& field, u64 expected, u64 actual, std::string reason = "") {
        if (expected != actual) {
            found.push_back({field, std::format("{:08X}", expected), std::format("{:08X}", actual),
                             std::move(reason)});
        }
    };
    const std::array<double, 4> scales = getLaneScales(vu, readHex(row["upper"]));
    const std::string& lform = row["lform"].getText();
    const Expected expected = readExpected(row);

    const u32 pc = readHex(row["pc"]);
    VuMicro micro(vu, memory, unit.index, nullptr);
    micro.begin(pc);
    micro.step();

    // A vector off by rounding or by PCSX2's clamp leaves the flags it set off
    // the same way.
    std::string flagReason;
    auto compareVector = [&](const std::string& field, const Reg128& want, const Reg128& got) {
        if (want.lo == got.lo && want.hi == got.hi) {
            return;
        }
        const std::string reason = getVectorReason(want, got, scales);
        if (!reason.empty()) {
            flagReason = reason;
        }
        found.push_back({field, formatReg128(want), formatReg128(got), reason});
    };
    for (u32 i = 1; i < 32; ++i) {
        compareVector(std::format("vf{}", i), expected.vf[i], vu.vf[i]);
    }
    compareVector("acc", expected.acc, vu.acc);
    const std::string interfaceReason = kInterfaceForms.contains(lform) ? "interface" : "";
    for (u32 i = 1; i < 16; ++i) {
        compare(std::format("vi{}", i), expected.vi[i] & 0xFFFFu, vu.vi[i] & 0xFFFFu,
                interfaceReason);
    }
    compare("I", expected.vi[Vu::kI], vu.vi[Vu::kI]);
    compare("R", expected.vi[Vu::kR] & 0x7FFFFFu, vu.vi[Vu::kR] & 0x7FFFFFu);

    // The outer product leaves w alone, and PCSX2 leaves that lane's flags from
    // whatever ran before rather than clearing them.
    constexpr u32 kMacLaneW = 0x1111;
    const Json& out = row["pipeOut"];
    const u32 expectedMac = readHex(out["mac"]);
    if (flagReason.empty() && kOuterProductForms.contains(row["uform"].getText()) &&
        ((expectedMac ^ vu.vi[Vu::kMac]) & ~kMacLaneW) == 0) {
        flagReason = "stale w flags";
    }
    compare("mac", expectedMac, vu.vi[Vu::kMac], flagReason);
    compare("status", readHex(out["status"]) & 0xFu, vu.vi[Vu::kStatus] & 0xFu, flagReason);
    compare("clip", readHex(out["clip"]), vu.clip);

    for (const Json& quad : row["mem"].getItems()) {
        const std::string actual = readQuadword(memory, unit.data + readHex(quad["addr"]) * 16);
        if (actual != quad["after"].getText()) {
            found.push_back({"mem" + quad["addr"].getText(), quad["after"].getText(), actual, ""});
        }
    }

    const bool inDelaySlot = row["pipeIn"]["branch"]["count"].getInteger() != 0;
    if (kBranchForms.contains(lform) && !inDelaySlot) {
        micro.step();
        const Json& branch = out["branch"];
        const u32 target = branch["count"].getInteger() != 0
                               ? readHex(branch["pc"])
                               : (pc + 2) % unit.getInstructionCount();
        compare("branch", target, micro.getProgramCounter() % unit.getInstructionCount());
    }

    auto runUntilLanded = [&] {
        for (u32 i = 0; i < kLandingSteps; ++i) {
            micro.step();
        }
    };
    const Json& fdiv = out["fdiv"];
    if (kQuotientForms.contains(lform) && fdiv["enable"].getInteger() != 0 &&
        fdiv["age"].getInteger() == 0) {
        compare("divide flags", readHex(fdiv["status"]) & 0x30u, vu.vi[Vu::kStatus] & 0x30u);
        runUntilLanded();
        const u32 q = readHex(fdiv["reg"]);
        const u32 actual = vu.vi[Vu::kQ];
        compare("Q", q, actual,
                isHostLargest(q, actual) ? "host largest"
                : isRounding(q, actual, 0.0) ? "rounding" : "");
    }
    const Json& efu = out["efu"];
    if (kFunctionForms.contains(lform) && efu["enable"].getInteger() != 0 &&
        efu["age"].getInteger() == 0) {
        runUntilLanded();
        const u32 p = readHex(efu["reg"]);
        compare("P", p, vu.vi[Vu::kP], isRounding(p, vu.vi[Vu::kP], 0.0) ? "rounding" : "");
    }
    return found;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3 || (std::string(argv[2]) != "0" && std::string(argv[2]) != "1")) {
        spdlog::error("usage: test_vu_vectors <vuN_vectors.jsonl> <unit 0 or 1> [examples per form]");
        return 2;
    }
    const VuUnit unit = getVuUnit(static_cast<u32>(std::stoul(argv[2])));
    Report report(argc > 3 ? std::stoul(argv[3]) : 2);
    Memory memory = makeMainMemory();

    readRecords(argv[1], [&](const Json& row) {
        if (reachesOtherUnit(row)) {
            report.addKnown("the other unit's registers");
            return;
        }
        Vu vu;
        seed(vu, memory, unit, row);
        const std::string description =
            std::format("pc {} {} {} ({} | {})", row["pc"].getText(), row["upper"].getText(),
                        row["lower"].getText(), row["umnem"].getText(), row["lmnem"].getText());
        report.addCase(row["uform"].getText() + "/" + row["lform"].getText(), description,
                       check(vu, memory, unit, row));
    });
    return report.finish("cases");
}
