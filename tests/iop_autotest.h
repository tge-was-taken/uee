#pragma once

#include "autotest.h"
#include "iop_interpreter.h"
#include "iop_kernel.h"
#include "iop_run.h"
#include "iop_translate.h"
#include "irx.h"
#include "memory.h"
#include "stop.h"
#include "types.h"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <string>
#include <vector>

namespace uee::autotest {

inline int runIop(int argc, char** argv, IopFunctionRunner translated) {
    if (argc < 3) {
        spdlog::error("usage: {} <test.irx> <test.expected>", argv[0]);
        return 2;
    }

    const std::filesystem::path irxPath = argv[1];
    const std::string name = irxPath.filename().string();

    Memory memory = makeIopMemory();
    IopInterpreter interpreter(memory);
    const IrxModule module = loadIrx(readFile(irxPath), memory, kIopModuleBase);
    IopKernel kernel(interpreter.iop, memory, module.imports,
                     {module.program.imageEnd, kIopStackTop - kIopStackSize});
    enterModule(interpreter.iop, module);

    IopMachine machine{interpreter, memory, kernel, translated};
    const Stop stop = run(machine, kInstructionLimit);
    const std::string& actual = kernel.readOutput();
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
