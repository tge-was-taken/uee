#pragma once

#include "log.h"
#include "autotest.h"
#include "iop_interpreter.h"
#include "iop_kernel.h"
#include "iop_mmio.h"
#include "iop_run.h"
#include "iop_translate.h"
#include "irx.h"
#include "memory.h"
#include "stop.h"
#include "types.h"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <algorithm>
#include <span>
#include <string>
#include <vector>

namespace uee::autotest {

// The IOP scheduler runs an instruction slice, not a clock.
constexpr u64 kIopInstructionLimit = 500'000'000;

inline int runIop(int argc, char** argv, TranslatedIrx* translated) {
    configureLogging();
    if (argc < 3) {
        spdlog::error("usage: {} <test.irx> <test.expected>", argv[0]);
        return 2;
    }

    const std::filesystem::path irxPath = argv[1];
    const std::string name = irxPath.filename().string();

    Memory memory = makeIopMemory();
    Memory mainMemory = makeMainMemory();
    IopInterpreter interpreter(memory);
    std::vector<Import> imports;
    const u32 base = translated != nullptr ? translated->base : kIopModuleBase;
    const IrxModule module = loadIrx(readFile(irxPath), memory, base, {}, imports);
    if (translated != nullptr) {
        translated->importBase = static_cast<u32>(imports.size());
        for (const TranslatedImport& import : translated->imports) {
            imports.push_back(Import{std::string(import.module), import.id});
        }
    }
    std::string console;
    IopKernel kernel(interpreter.iop, memory, mainMemory, imports,
                     {module.program.imageEnd, kIopStackTop - kIopStackSize}, console);

    // A test that reaches a device register expects the machine's devices: a
    // pad that answers the same way every run, and a card with room for the
    // pages a test writes.
    struct RestingPad : PadSource {
        u16 buttons() override { return 0xFFFF; }
    } pad;
    struct SmallCard : CardStorage {
        std::vector<u8> bytes =
            std::vector<u8>(8 * (kCardPageBytes + kCardSpareBytes), 0xFF);

        void readPage(u32 page, std::span<u8> out) override {
            std::fill(out.begin(), out.end(), 0xFF);
            const usize at = static_cast<usize>(page) * (kCardPageBytes + kCardSpareBytes);
            for (usize i = 0; i < out.size() && at + i < bytes.size(); ++i) {
                out[i] = bytes[at + i];
            }
        }

        void writePage(u32 page, std::span<const u8> in) override {
            const usize at = static_cast<usize>(page) * (kCardPageBytes + kCardSpareBytes);
            for (usize i = 0; i < in.size() && at + i < bytes.size(); ++i) {
                bytes[at + i] = in[i];
            }
        }
    } card;
    IopMmio mmio;
    mmio.attach(memory, kernel);
    mmio.attachPad(&pad);
    mmio.attachCard(&card);
    enterModule(interpreter.iop, module);

    TranslatedIrx* active[1] = {translated};
    IopMachine machine{interpreter, memory, kernel,
                       translated != nullptr ? std::span<TranslatedIrx* const>(active)
                                             : std::span<TranslatedIrx* const>{}};
    const Symbols symbols;
    Stop stop = IopScheduler(machine, symbols).run(kIopInstructionLimit);
    if (stop.reason == StopReason::Returned) {
        stop.reason = StopReason::Exited;
    }
    const std::string& actual = console;
    const std::vector<u8> expectedBytes = readFile(argv[2]);
    const std::string expected(expectedBytes.begin(), expectedBytes.end());

    if (stop.reason != StopReason::Exited) {
        spdlog::error("{}: {}", name, describe(stop));
        reportDifferences(name, actual, expected);
        return 1;
    }

    if (readLines(actual) != readLines(expected)) {
        reportDifferences(name, actual, expected);
        return 1;
    }

    return 0;
}

}  // namespace uee::autotest
