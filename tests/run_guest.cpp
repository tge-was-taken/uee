#include "disc.h"
#include "elf.h"
#include "patches.h"
#include "interpreter.h"
#include "kernel.h"
#include "memory.h"
#include "run.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>


int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: run_guest <elf> [disc.iso] [patches.toml]\n");
        return 2;
    }

    std::ifstream file(argv[1], std::ios::binary);
    if (!file) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    const std::vector<u8> image{std::istreambuf_iterator<char>(file),
                                std::istreambuf_iterator<char>()};

    uee::Memory memory = uee::makeMainMemory();
    uee::Interpreter interpreter(memory);
    uee::Kernel kernel(interpreter.ee, memory);
    interpreter.ee.cpu.pc = uee::loadElf(image, memory).entry;
    if (argc > 3) {
        try {
            uee::applyPatches(memory, uee::loadPatches(argv[3]));
        } catch (const std::exception& error) {
            std::fprintf(stderr, "%s\n", error.what());
            return 2;
        }
    }

    uee::Disc disc{argc > 2 ? argv[2] : ""};
    if (disc.isOpen()) {
        kernel.attachDisc(disc);
    } else if (argc > 2) {
        std::fprintf(stderr, "cannot open disc %s\n", argv[2]);
        return 2;
    }

    uee::Machine machine{interpreter, memory, kernel, nullptr};
    const uee::Stop stop = uee::run(machine, 2'000'000'000);

    std::fputs(kernel.readOutput().c_str(), stdout);
    std::fprintf(stderr, "[%s]\n", uee::describe(stop).c_str());
    return stop.reason == uee::StopReason::Exited ? 0 : 1;
}
