#include "log.h"
#include "iop.h"

#include "iop_run.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <stdexcept>

namespace uee {
namespace {

LoggerChannel log("iop");

// A module that does not reach a blocking point in this many instructions is
// waiting on hardware nothing here drives, and would starve every thread the
// modules after it start.
constexpr u64 kInstructionLimit = 20'000'000;
constexpr u32 kFirstArgumentRegister = 4;
constexpr u32 kResultRegister = 2;

}  // namespace

Iop::Iop(Memory& mainMemory, std::string& console, std::span<TranslatedIrx* const> translated,
         std::span<const std::string> hostModules)
    : mKernel(mInterpreter.iop, mMemory, mainMemory, mImports,
              {kIopHeapBase, kIopStackTop - kIopStackSize}, console),
      mTranslated(translated), mHostModules(hostModules.begin(), hostModules.end()) {
    mMmio.attach(mMemory, mKernel);
    for (const TranslatedIrx* module : mTranslated) {
        mNextBase = std::max(mNextBase, (module->end + 0xFu) & ~0xFu);
    }
}

// "cdrom0:\\LIB31\\PADMAN.IRX" is configured as padman.
ModuleSource Iop::sourceOf(const std::string& name) const {
    std::string bare = name.substr(name.find_last_of(":\\/") + 1);
    bare = bare.substr(0, bare.find('.'));
    for (char& letter : bare) {
        letter = static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
    }
    for (const std::string& host : mHostModules) {
        if (host == bare) {
            return ModuleSource::Host;
        }
    }
    return ModuleSource::Guest;
}

TranslatedIrx* Iop::translationOf(const std::string& name) const {
    for (TranslatedIrx* module : mTranslated) {
        if (module->name == name) {
            return module;
        }
    }
    return nullptr;
}

// A started thread only runs once the entry that started it is out of the way,
// so every entry into the IOP is followed by letting the rest of them reach
// their first blocking point.
bool Iop::settle(IopScheduler::Skipping skipping) {
    mKernel.exitHostThread();
    const Stop stop = mScheduler.run(kInstructionLimit, skipping);
    if (stop.reason == StopReason::Deadlocked) {
        return true;
    }
    log.stub("iop settle: {}", describe(stop));
    return false;
}

bool Iop::dispatchPendingHandlers() {
    const std::vector<IopHandlerCall> pending = mKernel.takePendingHandlers();
    for (const IopHandlerCall& call : pending) {
        runHandler(call);
    }
    return !pending.empty();
}

void Iop::runHandler(const IopHandlerCall& call) {
    log.trace("handler {:08x}({:08x})", call.handler, call.argument);
    const usize interrupted = mKernel.enterInterruptThread();
    ++mHandlersRun;
    R3000A& cpu = mInterpreter.iop.cpu;
    cpu.pc = call.handler;
    cpu.gpr[reg::gp] = call.gp;
    cpu.gpr[reg::sp] = kIopStackTop - kIopStackSize / 2;
    cpu.gpr[reg::ra] = kIopModuleReturn;
    cpu.gpr[kFirstArgumentRegister] = call.argument;
    const Stop stop = mScheduler.run(kInstructionLimit);
    if (stop.reason == StopReason::ReachedLimit) {
        log.stub("interrupt handler {:08x} did not return", call.handler);
    }
    mKernel.leaveInterruptThread(interrupted);
}

Stop Iop::enter(IopScheduler::Skipping skipping) {
    for (;;) {
        Stop stop = mScheduler.run(kInstructionLimit, skipping);
        if (stop.reason == StopReason::Deadlocked && dispatchPendingHandlers()) {
            continue;
        }
        if (stop.reason == StopReason::Returned) {
            stop.reason = StopReason::Exited;
        }
        return stop;
    }
}

bool Iop::loadModule(const std::string& name, std::span<const u8> image) {
    // Loading it would bind later imports to its exports, not to the host.
    if (sourceOf(name) == ModuleSource::Host) {
        log.trace("{} is answered here, so its image is not loaded", name);
        return true;
    }
    TranslatedIrx* const translated = translationOf(name);
    const u32 base = translated != nullptr ? translated->base : mNextBase;
    const IrxModule module = loadIrx(image, mMemory, base, mLoaded, mImports);
    if (module.program.imageEnd > kIopHeapBase) {
        throw std::runtime_error(std::format(
            "the module loaded at {:08x} ends at {:08x}, past the {:08x} the heap starts at",
            base, module.program.imageEnd, kIopHeapBase));
    }
    // Translated for a different image, it would name addresses past its end.
    if (translated != nullptr && module.program.imageEnd != translated->end) {
        throw std::runtime_error(std::format(
            "{} was translated as {:08x}..{:08x} but the image loaded here ends at {:08x}",
            name, translated->base, translated->end, module.program.imageEnd));
    }
    mSymbols.load(name, base, module.program.imageEnd - base);
    if (translated == nullptr) {
        mNextBase = (module.program.imageEnd + 0xF) & ~0xFu;
    }
    mLoaded.push_back(module);
    if (translated != nullptr) {
        translated->importBase = static_cast<u32>(mImports.size());
        for (const TranslatedImport& import : translated->imports) {
            mImports.push_back(Import{std::string(import.module), import.id});
        }
        mActive.push_back(translated);
        mMachine.modules = mActive;
    }

    const u32 sequence = mKernel.sequence();
    mKernel.enterHostThread();
    enterModule(mInterpreter.iop, module);
    const Stop stop = enter();
    if (stop.reason != StopReason::Exited || !settle()) {
        log.stub("iop module entry: {}", describe(stop));
        mKernel.dropThreadsAbove(sequence);
        if (translated != nullptr) {
            mActive.pop_back();
            mMachine.modules = mActive;
        }
        return false;
    }
    return true;
}

void Iop::startVblank() {
    mKernel.raiseVblankStart();
}

void Iop::endVblank() {
    mKernel.raiseVblankEnd();
}

// The EE reaches here from the run loop it also calls into the IOP from, so a
// tick can land while a call is still parked on a coroutine. Resuming that
// coroutine again would run one thread twice; the IOP is running anyway.
void Iop::advance(u64 instructions) {
    if (mAdvancing) {
        return;
    }
    mAdvancing = true;
    mKernel.advanceTime(instructions);
    // The host thread is only the EE's way in. Leaving it runnable would hand
    // every slice straight back, since it sits on the address a call returns to.
    mKernel.exitHostThread();
    dispatchPendingHandlers();
    const Stop stop = mScheduler.run(instructions, IopScheduler::Skipping::Blocked);
    mAdvancing = false;
    if (stop.reason == StopReason::Unimplemented || stop.reason == StopReason::Undecodable ||
        stop.reason == StopReason::UnknownSyscall ||
        stop.reason == StopReason::UnmappedAccess) {
        log.stub("iop advance: {}", describe(stop));
    }
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

    // A call is part of the running machine, so the devices around it keep their
    // own pace rather than being jumped forward to whatever would unblock this.
    const Stop stop = enter(IopScheduler::Skipping::Blocked);
    if (stop.reason != StopReason::Exited) {
        log.stub("iop call {:08x}({:08x}, {:08x}, {:08x}): {}", address,
                 arguments.size() > 0 ? arguments[0] : 0, arguments.size() > 1 ? arguments[1] : 0,
                 arguments.size() > 2 ? arguments[2] : 0, describe(stop));
        return 0;
    }
    const u32 result = cpu.gpr[kResultRegister];
    settle(IopScheduler::Skipping::Blocked);
    return result;
}

}  // namespace uee
