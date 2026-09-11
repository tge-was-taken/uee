#include "memory.h"
#include "vectors.h"
#include "vu.h"
#include "vu_micro.h"

#include <spdlog/spdlog.h>

#include <format>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using namespace uee;
using namespace uee::test;

// Whole programs PCSX2 ran with its timing traced, run here from the same start
// with the VIF's writes placed where PCSX2 saw them.

constexpr u32 kItop = 0xD0, kTop = 0xE0;

void writeBytes(Memory& memory, u32 base, const std::string& bytes) {
    for (usize at = 0; at + 32 <= bytes.size(); at += 32) {
        writeQuadword(memory, base + static_cast<u32>(at / 2), bytes.substr(at, 32));
    }
}

void writeQuadwords(Memory& memory, u32 base, const Json& writes) {
    for (const Json& write : writes.getItems()) {
        writeQuadword(memory, base + readHex(write["q"]) * 16, write["v"].getText());
    }
}

// What PCSX2 says the registers hold, kept up to date step by step, and the
// memory the latest step wrote.
struct Expected {
    Reg128 vf[32];
    u32 vi[32];
    Reg128 acc;
    std::map<u32, std::string> quads;

    void apply(const Json& record) {
        quads.clear();
        if (const Json* deltas = record.find("d")) {
            for (const Json& delta : deltas->getItems()) {
                const std::string& file = delta["f"].getText();
                const u32 n = readInteger(delta["n"]);
                if (file == "vf") {
                    vf[n] = readReg128(delta["v"]);
                } else if (file == "vi") {
                    vi[n] = readHex(delta["v"]);
                } else {
                    acc = readReg128(delta["v"]);
                }
            }
        }
        if (const Json* writes = record.find("m")) {
            for (const Json& write : writes->getItems()) {
                quads[readHex(write["q"])] = write["v"].getText();
            }
        }
    }
};

// PCSX2 rounds on the host and this runtime follows the console, so float
// lanes a few places apart are the two disagreeing about rounding.
constexpr u32 kRoundingUlps = 64;

bool isRounding(const Reg128& expected, const Reg128& actual) {
    for (s32 i = 0; i < 4; ++i) {
        const u32 e = expected.word(i), a = actual.word(i);
        if (((e ^ a) & 0x80000000u) != 0 && ((e | a) & 0x7FFFFFFFu) != 0) {
            return false;
        }
        const u32 me = e & 0x7FFFFFFFu, ma = a & 0x7FFFFFFFu;
        if ((me > ma ? me - ma : ma - me) > kRoundingUlps) {
            return false;
        }
    }
    return true;
}

bool isRounding(const std::string& expected, const std::string& actual) {
    Reg128 e, a;
    for (s32 i = 0; i < 4; ++i) {
        e.setWord(i, readLittleEndianWord(expected, static_cast<u32>(i)));
        a.setWord(i, readLittleEndianWord(actual, static_cast<u32>(i)));
    }
    return isRounding(e, a);
}

void compareVector(std::vector<Mismatch>& found, const std::string& field, const Reg128& expected,
                   const Reg128& actual) {
    if (expected.lo != actual.lo || expected.hi != actual.hi) {
        found.push_back({field, formatReg128(expected), formatReg128(actual),
                         isRounding(expected, actual) ? "rounding" : ""});
    }
}

// The flag registers are left out: PCSX2 records the readable copy four cycles
// late. A read of a flag still shows up in the integer register it lands in.
std::vector<Mismatch> compare(const Expected& expected, const Vu& vu, Memory& memory,
                              const VuUnit& unit) {
    std::vector<Mismatch> found;
    for (u32 i = 1; i < 32; ++i) {
        compareVector(found, std::format("vf{}", i), expected.vf[i], vu.vf[i]);
    }
    compareVector(found, "acc", expected.acc, vu.acc);
    for (u32 i = 1; i < 16; ++i) {
        if ((expected.vi[i] & 0xFFFFu) != (vu.vi[i] & 0xFFFFu)) {
            found.push_back({std::format("vi{}", i), std::format("{:04X}", expected.vi[i] & 0xFFFFu),
                             std::format("{:04X}", vu.vi[i] & 0xFFFFu), ""});
        }
    }
    if (expected.vi[Vu::kI] != vu.vi[Vu::kI]) {
        found.push_back({"I", std::format("{:08X}", expected.vi[Vu::kI]),
                         std::format("{:08X}", vu.vi[Vu::kI]), ""});
    }
    for (const auto& [quad, bytes] : expected.quads) {
        const std::string actual = readQuadword(memory, unit.data + quad * 16);
        if (actual != bytes) {
            found.push_back({std::format("mem{:03X}", quad), bytes, actual,
                             isRounding(bytes, actual) ? "rounding" : ""});
        }
    }
    return found;
}

// PCSX2 lands Q and P at the start of the reader and we at the end of the step
// before, so ours is compared with the next step's record, except across stalls.
void compareDelivered(std::vector<Mismatch>& found, const char* name, u32 expected, u32 actual,
                      bool stalled) {
    if (stalled || expected == actual) {
        return;
    }
    const bool hostLargest = (expected & 0x7FFFFFFFu) == 0x7F7FFFFFu &&
                             (actual & 0x7FFFFFFFu) == 0x7FFFFFFFu;
    const bool rounding = hostLargest || isRounding(Reg128{expected, 0}, Reg128{actual, 0});
    found.push_back({name, std::format("{:08X}", expected), std::format("{:08X}", actual),
                     rounding ? "rounding" : ""});
}

// So one difference is reported once instead of in every step after it. Q and P
// stay with the pipelines, whose timing is what this checks.
void resync(const Expected& expected, Vu& vu, Memory& memory, const VuUnit& unit) {
    for (u32 i = 1; i < 32; ++i) {
        vu.vf[i] = expected.vf[i];
    }
    vu.acc = expected.acc;
    for (u32 i = 1; i < 16; ++i) {
        vu.vi[i] = expected.vi[i] & 0xFFFFu;
    }
    vu.vi[Vu::kI] = expected.vi[Vu::kI];
    for (const auto& [quad, bytes] : expected.quads) {
        writeQuadword(memory, unit.data + quad * 16, bytes);
    }
}

std::unordered_map<std::string, std::string> readImages(const std::string& path) {
    std::unordered_map<std::string, std::string> images;
    readRecords(path, [&](const Json& row) {
        images[row["image"].getText()] = row["micro"].getText();
    });
    return images;
}

void replay(Memory& memory, const Json& run, const std::string& image, Report& report) {
    const VuUnit unit = getVuUnit(readInteger(run["unit"]));
    writeBytes(memory, unit.micro, image);
    writeBytes(memory, unit.data, run["mem"].getText());
    memory.write32(unit.interface + kTop, readHex(run["vif"]["top"]));
    memory.write32(unit.interface + kItop, readHex(run["vif"]["itop"]));

    Vu vu;
    Expected expected;
    const Json& in = run["in"];
    for (u32 i = 0; i < 32; ++i) {
        vu.vf[i] = expected.vf[i] = readReg128(in["vf"][i]);
        vu.vi[i] = expected.vi[i] = readHex(in["vi"][i]);
    }
    vu.loadAcc(readReg128(in["acc"]));
    expected.acc = vu.acc;
    vu.syncFlags();
    vu.clip = vu.vi[Vu::kClip];

    VuMicro micro(vu, memory, unit.index, nullptr);
    micro.begin(readHex(run["entry"]));
    u32 ourQ = vu.vi[Vu::kQ], ourP = vu.vi[Vu::kP];
    const std::string imageName = run["image"].getText().substr(0, 8);
    const std::vector<Json>& steps = run["steps"].getItems();
    for (usize s = 0; s < steps.size(); ++s) {
        const Json& step = steps[s];
        if (const Json* external = step.find("ext")) {
            if (const Json* writes = external->find("m")) {
                writeQuadwords(memory, unit.data, *writes);
            }
            expected.apply(*external);
            continue;
        }
        const std::string site = imageName + ":" + step["pc"].getText();
        const std::string description = std::format("entry {} step {}", run["entry"].getText(), s);
        const u32 pc = micro.getProgramCounter() % unit.getInstructionCount();
        if (pc != readHex(step["pc"])) {
            report.addCase(site, description,
                           {{"pc", step["pc"].getText(), std::format("{:04X}", pc), ""}});
            return;
        }
        const u32 before = micro.getCycle();
        micro.step();
        expected.apply(step);
        std::vector<Mismatch> found = compare(expected, vu, memory, unit);
        const u32 expectedCycles = readInteger(step["c"]);
        const bool stalled = expectedCycles != 1;
        compareDelivered(found, "Q", expected.vi[Vu::kQ], ourQ, stalled);
        compareDelivered(found, "P", expected.vi[Vu::kP], ourP, stalled);
        ourQ = vu.vi[Vu::kQ];
        ourP = vu.vi[Vu::kP];
        resync(expected, vu, memory, unit);
        if (micro.getCycle() - before != expectedCycles) {
            found.push_back({"cycles", std::to_string(expectedCycles),
                             std::to_string(micro.getCycle() - before), ""});
        }
        report.addCase(site, description, found);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        spdlog::error("usage: test_vu_runs <scene directory> [examples per site]");
        return 2;
    }
    const std::string directory = argv[1];
    const std::unordered_map<std::string, std::string> images =
        readImages(directory + "/vu_run_images.jsonl");
    Memory memory = makeMainMemory();
    Report report(argc > 2 ? std::stoul(argv[2]) : 1);
    usize runs = 0, clean = 0;
    readRecords(directory + "/vu_runs.jsonl", [&](const Json& run) {
        const usize failedBefore = report.getFailed();
        replay(memory, run, images.at(run["image"].getText()), report);
        ++runs;
        clean += report.getFailed() == failedBefore;
    });
    std::printf("%zu of %zu runs match every step\n", clean, runs);
    return report.finish("steps");
}
