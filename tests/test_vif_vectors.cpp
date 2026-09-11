#include "gif.h"
#include "gs.h"
#include "memory.h"
#include "vectors.h"
#include "vif.h"
#include "vu.h"

#include <spdlog/spdlog.h>

#include <format>
#include <string>
#include <vector>

namespace {

using namespace uee;
using namespace uee::test;

// Each record is one VIFcode as PCSX2 ran it. The registers it starts from go in
// the way a program sets them, through codes of their own.

constexpr u32 kStcycl = 0x01, kOffset = 0x02, kBase = 0x03, kItop = 0x04, kStmod = 0x05;
constexpr u32 kMskpath3 = 0x06, kMscal = 0x14, kMscnt = 0x17;
constexpr u32 kStmask = 0x20, kStrow = 0x30, kStcol = 0x31;

constexpr u32 kEndBit = 1u << 30;
constexpr u32 kNopUpper = 0x000002FF;
constexpr u32 kNopLower = 0x8000033C;
constexpr u32 kDoubleBuffer = 1u << 7;

constexpr u32 kStat = 0x00, kCycle = 0x40, kMode = 0x50, kNum = 0x60, kMask = 0x70;
constexpr u32 kCode = 0x80, kItops = 0x90, kBaseRegister = 0xA0, kOffsetRegister = 0xB0;
constexpr u32 kTops = 0xC0, kItopRegister = 0xD0, kTop = 0xE0, kRow = 0x100, kColumn = 0x140;

constexpr u32 encodeCode(u32 command, u32 immediate = 0) { return (command << 24) | immediate; }

// A program that ends at once, so starting one changes nothing but the buffer
// it points the next unpack at.
void plantEmptyProgram(Memory& memory, const VuUnit& unit, u32 start) {
    for (u32 i = 0; i < 2; ++i) {
        const u32 at = unit.micro + ((start + i) % unit.getInstructionCount()) * 8;
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

// Both fill whole quadwords, so the memory can be put back to how the record
// found it between the setup and the code under test.
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

Stream buildStream(const Json& row, const VuUnit& unit) {
    const Json& in = row["regsIn"];
    Stream stream;
    std::vector<u32>& words = stream.setup;
    words.push_back(encodeCode(kStcycl, (readInteger(in["wl"]) << 8) | readInteger(in["cl"])));
    words.push_back(encodeCode(kStmod, readInteger(in["mode"])));
    words.push_back(encodeCode(kStmask));
    words.push_back(readHex(in["mask"]));
    words.push_back(encodeCode(kStrow));
    for (u32 i = 0; i < 4; ++i) {
        words.push_back(readHex(in["maskRow"][i]));
    }
    words.push_back(encodeCode(kStcol));
    for (u32 i = 0; i < 4; ++i) {
        words.push_back(readHex(in["maskCol"][i]));
    }
    if (unit.index == 1) {
        // An offset starts the first buffer at the base, and a swap moves on
        // by the offset; the base itself can change after either.
        const bool second = (readHex(in["stat"]) & kDoubleBuffer) != 0;
        const u32 offset = readHex(in["ofst"]);
        const u32 start = (readHex(in["tops"]) - (second ? offset : 0)) & 0x3FFu;
        words.push_back(encodeCode(kBase, start));
        words.push_back(encodeCode(kOffset, offset));
        if (second) {
            words.push_back(encodeCode(kMscal, unit.getInstructionCount() - 4));
        }
        words.push_back(encodeCode(kBase, readHex(in["base"])));
        words.push_back(encodeCode(kMskpath3, readHex(in["mskpath3"]) != 0 ? 0x8000u : 0u));
    }
    words.push_back(encodeCode(kItop, readHex(in["itops"])));
    padToQuadword(words);

    // Data behind a DIRECT or MPG starts on a quadword, so the code goes last
    // in one.
    stream.command.assign(3, 0);
    for (const Json& word : row["payload"].getItems()) {
        stream.command.push_back(readHex(word));
    }
    // The last element of a three-lane unpack takes its fourth from what
    // follows.
    const usize end = stream.command.size();
    if (isThreeElementUnpack(row["form"].getText())) {
        if (const Json* tail = row.find("tail")) {
            stream.command.push_back(readLittleEndianWord(tail->getText(), 0));
            stream.hasTail = true;
        }
    }
    padToQuadword(stream.command);
    stream.hasTrailingWords = stream.command.size() != end;
    return stream;
}

// PCSX2's recompiled unpacker zeroes the lane after a V2 or V3 element by where
// it sits in the data; the console writes the next value, as we do.
bool isZeroedFourthLane(const std::string& form, const std::string& expected,
                        const std::string& actual) {
    const bool shortVector = form.find("UNPACK_V2") != std::string::npos ||
                             form.find("UNPACK_V3") != std::string::npos;
    return shortVector && expected.substr(0, 24) == actual.substr(0, 24) &&
           expected.substr(24) == "00000000";
}

void writeQuadwords(Vif& vif, const std::vector<u32>& words) {
    for (usize i = 0; i < words.size(); i += 4) {
        vif.writeQuadword(static_cast<u64>(words[i]) | static_cast<u64>(words[i + 1]) << 32,
                          static_cast<u64>(words[i + 2]) | static_cast<u64>(words[i + 3]) << 32);
    }
}

void restoreMemory(Memory& memory, u32 base, const Json& quads) {
    for (const Json& quad : quads.getItems()) {
        writeQuadword(memory, base + readHex(quad["addr"]) * 16, quad["before"].getText());
    }
}

void checkMemory(Memory& memory, u32 base, const Json& quads, const std::string& field,
                 const std::string& form, std::vector<Mismatch>& found) {
    for (const Json& quad : quads.getItems()) {
        const std::string actual = readQuadword(memory, base + readHex(quad["addr"]) * 16);
        const std::string& expected = quad["after"].getText();
        if (actual != expected) {
            found.push_back({field + quad["addr"].getText(), expected, actual,
                             isZeroedFourthLane(form, expected, actual) ? "zeroed fourth lane" : ""});
        }
    }
}

void checkRegisters(Vif& vif, const VuUnit& unit, const Json& row, const Stream& stream,
                    std::vector<Mismatch>& found) {
    const std::string reason = stream.hasTail ? "tail runs as a code" : "";
    auto compare = [&](const std::string& field, u32 expected, u32 actual) {
        if (expected != actual) {
            found.push_back({field, std::format("{:08X}", expected), std::format("{:08X}", actual),
                             reason});
        }
    };
    auto readRegister = [&](u32 offset) { return vif.read(unit.interface + offset).value(); };

    const Json& out = row["regsOut"];
    const u32 cycle = readRegister(kCycle);
    compare("cl", readInteger(out["cl"]), cycle & 0xFFu);
    compare("wl", readInteger(out["wl"]), (cycle >> 8) & 0xFFu);
    compare("mode", readInteger(out["mode"]), readRegister(kMode));
    compare("mask", readHex(out["mask"]), readRegister(kMask));
    compare("itops", readHex(out["itops"]), readRegister(kItops));
    for (u32 i = 0; i < 4; ++i) {
        compare(std::format("row{}", i), readHex(out["maskRow"][i]), readRegister(kRow + i * 0x10));
        compare(std::format("col{}", i), readHex(out["maskCol"][i]), readRegister(kColumn + i * 0x10));
    }
    if (!stream.hasTrailingWords) {
        compare("num", readInteger(out["num"]), readRegister(kNum));
        compare("code", readHex(out["code"]), readRegister(kCode));
    }
    if (unit.index == 1) {
        compare("base", readHex(out["base"]), readRegister(kBaseRegister));
        compare("ofst", readHex(out["ofst"]), readRegister(kOffsetRegister));
        compare("tops", readHex(out["tops"]), readRegister(kTops));
        compare("dbf", readHex(out["stat"]) & kDoubleBuffer, readRegister(kStat) & kDoubleBuffer);
    }
    if (startsProgram(row["form"].getText())) {
        compare("top", readHex(out["top"]), readRegister(kTop));
        compare("itop", readHex(out["itop"]), readRegister(kItopRegister));
    }
}

std::vector<Mismatch> run(Memory& memory, const Json& row) {
    const VuUnit unit = getVuUnit(readInteger(row["vif"]));
    Gs gs;
    Gif gif;
    gif.attach(memory, gs);
    Vu vu;
    Vif vif;
    vif.attach(memory, gif, unit.index, &vu);

    plantEmptyProgram(memory, unit, unit.getInstructionCount() - 4);
    plantEmptyProgram(memory, unit, unit.getInstructionCount() - 2);
    // MSCNT carries on from where the last program stopped.
    vu.vi[Vu::kTpc] = unit.getInstructionCount() - 2;
    const std::string& form = row["form"].getText();
    const u32 code = readHex(row["code"]);
    if (startsProgram(form) && (code >> 24 & 0x7Fu) != kMscnt) {
        plantEmptyProgram(memory, unit, code & 0xFFFFu);
    }

    const Stream stream = buildStream(row, unit);
    writeQuadwords(vif, stream.setup);
    restoreMemory(memory, unit.data, row["mem"]);
    restoreMemory(memory, unit.micro, row["micro"]);
    writeQuadwords(vif, stream.command);

    std::vector<Mismatch> found;
    checkMemory(memory, unit.data, row["mem"], "mem", form, found);
    checkMemory(memory, unit.micro, row["micro"], "micro", form, found);
    checkRegisters(vif, unit, row, stream, found);
    return found;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        spdlog::error("usage: test_vif_vectors <vif_vectors.jsonl> [examples per form]");
        return 2;
    }
    Memory memory = makeMainMemory();
    Report report(argc > 2 ? std::stoul(argv[2]) : 2);
    readRecords(argv[1], [&](const Json& row) {
        if (row["payloadTruncated"].getBool()) {
            return;
        }
        const Json& in = row["regsIn"];
        report.addCase(row["form"].getText(),
                       std::format("code {} cl {} wl {} mode {}", row["code"].getText(),
                                   readInteger(in["cl"]), readInteger(in["wl"]), readInteger(in["mode"])),
                       run(memory, row));
    });
    return report.finish("cases");
}
