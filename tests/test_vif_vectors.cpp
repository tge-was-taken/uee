#include "gif.h"
#include "gs.h"
#include "json.h"
#include "memory.h"
#include "vector_memory.h"
#include "vif.h"
#include "vu.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <format>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace uee;
using test::Json;

// One VIFcode as PCSX2 ran it, from pcsx2-hook's PCSX2HOOK_VIF_CMD_TRACE: the
// registers either side, every word it consumed and the VU memory it wrote.
// The registers go in the way a program sets them, through codes of their own.

constexpr u32 kStcycl = 0x01, kOffset = 0x02, kBase = 0x03, kItop = 0x04, kStmod = 0x05;
constexpr u32 kMskpath3 = 0x06, kMscal = 0x14, kStmask = 0x20, kStrow = 0x30, kStcol = 0x31;

constexpr u32 kEndBit = 1u << 30;
constexpr u32 kNopUpper = 0x000002FF;
constexpr u32 kNopLower = 0x8000033C;
constexpr u32 kDoubleBuffer = 1u << 7;

constexpr u32 kStat = 0x00, kCycle = 0x40, kMode = 0x50, kNum = 0x60, kMask = 0x70;
constexpr u32 kCode = 0x80, kItops = 0x90, kBaseRegister = 0xA0, kOffsetRegister = 0xB0;
constexpr u32 kTops = 0xC0, kItopRegister = 0xD0, kTop = 0xE0, kRow = 0x100, kColumn = 0x140;

constexpr u32 code(u32 command, u32 immediate = 0) { return (command << 24) | immediate; }

struct Unit {
    u32 index;
    u32 registers;
    u32 data;
    u32 micro;
    u32 instructions;
};

Unit getUnit(u32 index) {
    if (index == 0) {
        return {0, 0x10003800, kVu0DataBase, kVu0MicroBase, kVu0MemorySize / 8};
    }
    return {1, 0x10003C00, kVu1DataBase, kVu1MicroBase, kVu1MemorySize / 8};
}

u32 hex(const Json& value) { return static_cast<u32>(value.getHex()); }
u32 integer(const Json& value) { return static_cast<u32>(value.getInteger()); }

// A program that ends at once, so starting one changes nothing but the buffer
// it points the next unpack at.
void plantEmptyProgram(Memory& memory, const Unit& unit, u32 start) {
    for (u32 i = 0; i < 2; ++i) {
        const u32 at = unit.micro + ((start + i) % unit.instructions) * 8;
        memory.write32(at, kNopLower);
        memory.write32(at + 4, i == 0 ? (kNopUpper | kEndBit) : kNopUpper);
    }
}

bool isThreeElementUnpack(const std::string& form) {
    return form.find("UNPACK_V3") != std::string::npos;
}

bool startsProgram(const std::string& form) {
    return form.find(":MSCAL") != std::string::npos || form.find(":MSCNT") != std::string::npos;
}

// The codes that set the registers the record starts from, and then the code
// under test. Each fills whole quadwords, so the memory the code writes can be
// put back to how the record found it once the setup has run.
struct Stream {
    std::vector<u32> setup;
    std::vector<u32> command;
    // Words behind the code under test run as codes of their own, which
    // leaves the code and count registers describing them instead.
    bool hasTrailingWords = false;
    // The word a three element unpack takes its last lane from can be any
    // code, and it runs once the unpack is done.
    bool hasTail = false;
};

void padToQuadword(std::vector<u32>& words) {
    while (words.size() % 4 != 0) {
        words.push_back(0);
    }
}

Stream buildStream(const Json& row, const Unit& unit) {
    const Json& in = row["regsIn"];
    Stream stream;
    std::vector<u32>& words = stream.setup;
    words.push_back(code(kStcycl, (integer(in["wl"]) << 8) | integer(in["cl"])));
    words.push_back(code(kStmod, integer(in["mode"])));
    words.push_back(code(kStmask));
    words.push_back(hex(in["mask"]));
    words.push_back(code(kStrow));
    for (u32 i = 0; i < 4; ++i) {
        words.push_back(hex(in["maskRow"][i]));
    }
    words.push_back(code(kStcol));
    for (u32 i = 0; i < 4; ++i) {
        words.push_back(hex(in["maskCol"][i]));
    }
    if (unit.index == 1) {
        // An offset starts the first buffer at the base, and a swap moves on
        // by the offset; the base itself can change after either.
        const bool second = (hex(in["stat"]) & kDoubleBuffer) != 0;
        const u32 offset = hex(in["ofst"]);
        const u32 start = (hex(in["tops"]) - (second ? offset : 0)) & 0x3FFu;
        words.push_back(code(kBase, start));
        words.push_back(code(kOffset, offset));
        if (second) {
            words.push_back(code(kMscal, unit.instructions - 4));
        }
        words.push_back(code(kBase, hex(in["base"])));
        words.push_back(code(kMskpath3, hex(in["mskpath3"]) != 0 ? 0x8000u : 0u));
    }
    words.push_back(code(kItop, hex(in["itops"])));
    padToQuadword(words);

    // Data behind a DIRECT or MPG starts on a quadword, so the code goes last
    // in one.
    stream.command.assign(3, 0);
    for (const Json& word : row["payload"].getItems()) {
        stream.command.push_back(hex(word));
    }
    // The last element of a three-lane unpack takes its fourth from what
    // follows.
    const usize end = stream.command.size();
    if (isThreeElementUnpack(row["form"].getText())) {
        if (const Json* tail = row.find("tail")) {
            stream.command.push_back(test::readLittleEndianWord(tail->getText(), 0));
            stream.hasTail = true;
        }
    }
    padToQuadword(stream.command);
    stream.hasTrailingWords = stream.command.size() != end;
    return stream;
}

// A mismatch with a reason is a known disagreement between PCSX2 and this
// runtime; one without is a failure.
struct Mismatch {
    std::string field;
    std::string expected;
    std::string actual;
    std::string reason;
};

// PCSX2's recompiled unpacker zeroes the lane after a two or three element
// vector depending on where the element sits in the data. The console records
// the next value there for the placements it has been tried at, and for V3-8
// contradicts the rule outright, so this runtime writes the next value.
bool isZeroedFourthLane(const std::string& form, const std::string& expected,
                        const std::string& actual) {
    const bool shortVector = form.find("UNPACK_V2") != std::string::npos ||
                             form.find("UNPACK_V3") != std::string::npos;
    return shortVector && expected.substr(0, 24) == actual.substr(0, 24) &&
           expected.substr(24) == "00000000";
}

std::vector<Mismatch> run(Memory& memory, const Json& row) {
    const Unit unit = getUnit(integer(row["vif"]));
    Gs gs;
    Gif gif;
    gif.attach(memory, gs);
    Vu vu;
    Vif vif;
    vif.attach(memory, gif, unit.index, &vu);

    plantEmptyProgram(memory, unit, unit.instructions - 4);
    plantEmptyProgram(memory, unit, unit.instructions - 2);
    // MSCNT carries on from where the last program stopped.
    vu.vi[Vu::kTpc] = unit.instructions - 2;
    const std::string& form = row["form"].getText();
    const u32 command = hex(row["code"]) >> 24 & 0x7Fu;
    if (startsProgram(form) && command != 0x17) {
        plantEmptyProgram(memory, unit, hex(row["code"]) & 0xFFFFu);
    }

    const Stream stream = buildStream(row, unit);
    auto feed = [&](const std::vector<u32>& words) {
        for (usize i = 0; i < words.size(); i += 4) {
            vif.writeQuadword(static_cast<u64>(words[i]) | static_cast<u64>(words[i + 1]) << 32,
                              static_cast<u64>(words[i + 2]) | static_cast<u64>(words[i + 3]) << 32);
        }
    };
    feed(stream.setup);

    for (const Json& quad : row["mem"].getItems()) {
        test::writeQuadword(memory, unit.data + hex(quad["addr"]) * 16, quad["before"].getText());
    }
    for (const Json& quad : row["micro"].getItems()) {
        test::writeQuadword(memory, unit.micro + hex(quad["addr"]) * 16, quad["before"].getText());
    }
    feed(stream.command);

    std::vector<Mismatch> found;
    const std::string registerReason = stream.hasTail ? "tail runs as a code" : "";
    auto compare = [&](const std::string& field, u32 expected, u32 actual) {
        if (expected != actual) {
            found.push_back({field, std::format("{:08X}", expected), std::format("{:08X}", actual),
                             registerReason});
        }
    };
    auto readRegister = [&](u32 offset) { return vif.read(unit.registers + offset).value_or(0xDEADBEEF); };

    for (const Json& quad : row["mem"].getItems()) {
        const std::string actual = test::readQuadword(memory, unit.data + hex(quad["addr"]) * 16);
        const std::string& expected = quad["after"].getText();
        if (actual != expected) {
            found.push_back({"mem" + quad["addr"].getText(), expected, actual,
                             isZeroedFourthLane(form, expected, actual) ? "zeroed fourth lane" : ""});
        }
    }
    for (const Json& quad : row["micro"].getItems()) {
        const std::string actual = test::readQuadword(memory, unit.micro + hex(quad["addr"]) * 16);
        if (actual != quad["after"].getText()) {
            found.push_back({"micro" + quad["addr"].getText(), quad["after"].getText(), actual, ""});
        }
    }

    const Json& out = row["regsOut"];
    const u32 cycle = readRegister(kCycle);
    compare("cl", integer(out["cl"]), cycle & 0xFFu);
    compare("wl", integer(out["wl"]), (cycle >> 8) & 0xFFu);
    compare("mode", integer(out["mode"]), readRegister(kMode));
    compare("mask", hex(out["mask"]), readRegister(kMask));
    compare("itops", hex(out["itops"]), readRegister(kItops));
    for (u32 i = 0; i < 4; ++i) {
        compare(std::format("row{}", i), hex(out["maskRow"][i]), readRegister(kRow + i * 0x10));
        compare(std::format("col{}", i), hex(out["maskCol"][i]), readRegister(kColumn + i * 0x10));
    }
    if (!stream.hasTrailingWords) {
        compare("num", integer(out["num"]), readRegister(kNum));
        compare("code", hex(out["code"]), readRegister(kCode));
    }
    if (unit.index == 1) {
        compare("base", hex(out["base"]), readRegister(kBaseRegister));
        compare("ofst", hex(out["ofst"]), readRegister(kOffsetRegister));
        compare("tops", hex(out["tops"]), readRegister(kTops));
        compare("dbf", hex(out["stat"]) & kDoubleBuffer, readRegister(kStat) & kDoubleBuffer);
    }
    if (startsProgram(form)) {
        compare("top", hex(out["top"]), readRegister(kTop));
        compare("itop", hex(out["itop"]), readRegister(kItopRegister));
    }
    return found;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        spdlog::error("usage: test_vif_vectors <vif_vectors.jsonl> [examples per form]");
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
        if (row["payloadTruncated"].getBool()) {
            continue;
        }
        const std::vector<Mismatch> found = run(memory, row);
        ++cases;
        std::set<std::string> fields;
        std::set<std::string> reasons;
        for (const Mismatch& m : found) {
            if (!m.reason.empty()) {
                reasons.insert(m.reason);
                continue;
            }
            std::string category;
            for (const char c : m.field) {
                if (c < '0' || c > '9') {
                    category += c;
                }
            }
            fields.insert(m.field.starts_with("mem") ? "mem" : m.field.starts_with("micro") ? "micro" : category);
        }
        for (const std::string& reason : reasons) {
            ++knownByReason[reason];
        }
        if (fields.empty()) {
            continue;
        }
        ++failed;
        for (const std::string& field : fields) {
            const std::string key = row["form"].getText() + " " + field;
            ++failuresByKey[key];
            std::vector<std::string>& list = examplesByKey[key];
            if (list.size() < examples) {
                std::string text = std::format("code {} cl {} wl {} mode {}", row["code"].getText(),
                                               integer(row["regsIn"]["cl"]),
                                               integer(row["regsIn"]["wl"]),
                                               integer(row["regsIn"]["mode"]));
                usize shown = 0;
                for (const Mismatch& m : found) {
                    if (shown++ == 6) {
                        text += std::format("\n      ... {} more", found.size() - 6);
                        break;
                    }
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
