#include "elf.h"
#include "guest_replacement.h"
#include "instruction.h"
#include "interpreter.h"
#include "log.h"
#include "memory.h"
#include "types.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

// Each replacement in guest_replacement.cpp against the guest code it stands in
// for, run on the same inputs through the interpreter. A transcription that
// reads correctly can still answer differently.
namespace {

uee::LoggerChannel report("replacements");

constexpr u32 kReturn = 0x0FFFFFF0;
constexpr u32 kLeft = 0x00300000;
constexpr u32 kRight = 0x00310000;
constexpr u32 kStrcmp = 0x00524128;

[[noreturn]] void fail(const std::string& message) {
    report.fatal("{}", message);
    std::exit(2);
}

std::vector<u8> readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        fail(std::format("cannot open {}", path.string()));
    }
    return std::vector<u8>(std::istreambuf_iterator<char>(file), {});
}

void runInterpreter(uee::Interpreter& interpreter, uee::Memory& memory, u32 entry) {
    interpreter.ee.cpu.pc = entry;
    for (u32 steps = 0; steps < 100000; ++steps) {
        const u32 pc = interpreter.ee.cpu.pc;
        if (pc == kReturn) {
            return;
        }
        const uee::Instruction insn{memory.read32(pc)};
        if (interpreter.execute(insn) != uee::Status::Executed) {
            fail(std::format("{:08x} did not execute", pc));
        }
        if (interpreter.hasDelaySlot()) {
            const u32 pending = interpreter.ee.cpu.pc;
            interpreter.ee.cpu.pc = pc + 4;
            const uee::Instruction slot{memory.read32(pc + 4)};
            if (interpreter.execute(slot) != uee::Status::Executed) {
                fail(std::format("{:08x} did not execute", pc + 4));
            }
            if (!interpreter.tookBranch()) {
                interpreter.ee.cpu.pc = pending;
            }
        }
    }
    fail(std::format("{:08x} did not return", entry));
}

void writeString(uee::Memory& memory, u32 at, const std::string& text) {
    for (usize i = 0; i < text.size(); ++i) {
        memory.write8(at + static_cast<u32>(i), static_cast<u8>(text[i]));
    }
    memory.write8(at + static_cast<u32>(text.size()), 0);
}

// The guest routine compares a quadword at a time, so the cases that matter are
// the ones straddling that: a difference past the first block, and a length
// that ends inside one.
std::pair<std::string, std::string> makePair(std::mt19937& rng) {
    const usize length = rng() % 40;
    std::string left;
    for (usize i = 0; i < length; ++i) {
        left.push_back(static_cast<char>('a' + rng() % 26));
    }
    std::string right = left;
    switch (rng() % 4) {
        case 0: break;
        case 1:
            if (!right.empty()) {
                right[rng() % right.size()] = static_cast<char>('a' + rng() % 26);
            }
            break;
        case 2:
            right.push_back(static_cast<char>('a' + rng() % 26));
            break;
        default:
            if (!right.empty()) {
                right.pop_back();
            }
            break;
    }
    return {left, right};
}

}  // namespace

int main(int argc, char** argv) {
    uee::configureLogging();
    if (argc < 2) {
        fail(std::format("usage: {} <elf> [rounds]", argv[0]));
    }
    const u32 rounds = argc > 2 ? static_cast<u32>(std::atoi(argv[2])) : 20000;

    uee::Memory memory = uee::makeMainMemory();
    uee::Interpreter interpreter(memory);
    uee::loadElf(readFile(argv[1]), memory);

    std::mt19937 rng(20260903);
    u32 mismatched = 0;
    for (u32 round = 0; round < rounds; ++round) {
        const auto [left, right] = makePair(rng);
        // Both alignments, since the guest takes a different path for each.
        const u32 leftAt = kLeft + rng() % 4;
        const u32 rightAt = kRight + rng() % 4;
        writeString(memory, leftAt, left);
        writeString(memory, rightAt, right);

        const auto setUp = [&] {
            interpreter.ee.cpu.write32(uee::reg::a0, leftAt);
            interpreter.ee.cpu.write32(uee::reg::a1, rightAt);
            interpreter.ee.cpu.write32(uee::reg::v0, 0xDEADBEEF);
            interpreter.ee.cpu.write32(uee::reg::ra, kReturn);
        };

        setUp();
        runInterpreter(interpreter, memory, kStrcmp);
        const u32 guest = static_cast<u32>(interpreter.ee.cpu.gpr[uee::reg::v0].lo);

        setUp();
        uee::guestlib::strcmp(interpreter.ee.cpu, memory);
        const u32 host = static_cast<u32>(interpreter.ee.cpu.gpr[uee::reg::v0].lo);

        if (guest != host) {
            report.error(R"(round {}: "{}" vs "{}" guest {:08x} host {:08x})", round, left,
                         right, guest, host);
            if (++mismatched >= 5) {
                fail("strcmp does not match the guest");
            }
        }
    }

    if (mismatched != 0) {
        fail("strcmp does not match the guest");
    }
    report.info("strcmp matches over {} rounds", rounds);
    return 0;
}
