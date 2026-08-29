#include "log.h"
#include "iop.h"

#include "iop_run.h"

#include <format>

namespace uee {
namespace {

LoggerChannel log("iop");

// Past this a module is waiting on hardware nothing here drives, and would
// starve every thread the modules after it start.
constexpr u64 kInstructionLimit = 20'000'000;
constexpr u32 kFirstArgumentRegister = 4;
constexpr u32 kResultRegister = 2;

}  // namespace

Iop::Iop(std::string& console)
    : mKernel(mInterpreter.iop, mMemory, mImports,
              {kIopHeapBase, kIopStackTop - kIopStackSize}, console) {}

// A started thread only runs once the entry that started it is out of the way.
bool Iop::settle() {
    mKernel.exitCurrentThread();
    IopMachine machine{mInterpreter, mMemory, mKernel, nullptr};
    const Stop stop = run(machine, kInstructionLimit, mSymbols);
    if (stop.reason == StopReason::Deadlocked) {
        return true;
    }
    log.stub("settle: {}", describe(stop));
    return false;
}

Stop Iop::enter() {
    IopMachine machine{mInterpreter, mMemory, mKernel, nullptr};
    return run(machine, kInstructionLimit, mSymbols);
}

bool Iop::loadModule(const std::string& name, std::span<const u8> image) {
    const IrxModule module = loadIrx(image, mMemory, mNextBase, mLoaded, mImports);
    if (module.program.imageEnd > kIopHeapBase) {
        throw std::runtime_error(std::format(
            "the module loaded at {:08x} ends at {:08x}, past the {:08x} the heap starts at",
            mNextBase, module.program.imageEnd, kIopHeapBase));
    }
    mSymbols.load(name, mNextBase, module.program.imageEnd - mNextBase);
    mNextBase = (module.program.imageEnd + 0xF) & ~0xFu;
    mLoaded.push_back(module);

    const u32 sequence = mKernel.sequence();
    mKernel.enterHostThread();
    enterModule(mInterpreter.iop, module);
    const Stop stop = enter();
    if (stop.reason != StopReason::Exited || !settle()) {
        log.stub("module entry: {}", describe(stop));
        mKernel.dropThreadsAbove(sequence);
        return false;
    }
    return true;
}

// The EE's field is the only clock these modules get. Releasing the threads that
// wait on it is enough; they run on the next call, which is soon enough because
// nothing is waiting on an answer here.
void Iop::tickVblank() {
    mKernel.raiseVblank();
}

u32 Iop::callFunction(u32 address, u32 gp, std::span<const u32> arguments) {
    mKernel.enterHostThread();
    R3000A& cpu = mInterpreter.iop.cpu;
    cpu.pc = address;
    cpu.gpr[reg::gp] = gp;
    cpu.gpr[reg::sp] = kIopStackTop;
    cpu.gpr[reg::ra] = kIopModuleReturn;
    for (usize i = 0; i < arguments.size() && i < 4; ++i) {
        cpu.gpr[kFirstArgumentRegister + i] = arguments[i];
    }

    const Stop stop = enter();
    if (stop.reason != StopReason::Exited) {
        log.stub("call {:08x}: {}", address, describe(stop));
        return 0;
    }
    const u32 result = cpu.gpr[kResultRegister];
    settle();
    return result;
}

}  // namespace uee
