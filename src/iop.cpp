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

// Past this a module is waiting on hardware nothing here drives.
constexpr u64 kInstructionLimit = 20'000'000;
constexpr u32 kFirstArgumentRegister = 4;
constexpr u32 kResultRegister = 2;

}  // namespace

Iop::Iop(Memory& mainMemory, std::string& console, std::span<TranslatedIrx* const> translated,
         std::span<const std::string> hostModules, std::span<const u32> watched)
    : mKernel(mInterpreter.iop, mMemory, mainMemory, mImports,
              {kIopHeapBase, kIopStackTop - kIopStackSize}, console),
      mWatched(watched.begin(), watched.end()), mTranslated(translated),
      mHostModules(hostModules.begin(), hostModules.end()) {
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

// A started thread only runs once the entry that started it is out of the way.
bool Iop::sync(IopScheduler::Skipping skipping) {
    mKernel.exitHostThread();
    const Stop stop = mScheduler.run(kInstructionLimit, skipping);
    if (stop.reason == StopReason::Deadlocked) {
        return true;
    }
    log.stub("iop sync: {}", describe(stop));
    return false;
}

bool Iop::dispatchPendingHandlers() {
    const std::vector<IopHandlerCall> pending = mKernel.takePendingHandlers();
    for (const IopHandlerCall& call : pending) {
        runHandler(call);
    }
    return !pending.empty();
}

// What the processor was holding when it stopped, so a fault on a computed
// address says which register carried it.
void Iop::reportRegisters() const {
    const R3000A& cpu = mInterpreter.iop.cpu;
    std::string text;
    for (u32 i = 1; i < 32; ++i) {
        text += std::format("{}={:08x} ", reg::nameOf(i), cpu.gpr[i]);
    }
    log.info("at {} {}", mSymbols.describe(cpu.pc), text);
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

void Iop::indexModulePages() {
    u32 end = kIopModuleBase;
    for (const TranslatedIrx* module : mActive) {
        end = std::max(end, module->end);
    }
    mModulePages.assign((end - kIopModuleBase) / kModulePageBytes + 1, nullptr);
    for (TranslatedIrx* module : mActive) {
        for (u32 pc = module->base; pc < module->end; pc += kModulePageBytes) {
            mModulePages[(pc - kIopModuleBase) / kModulePageBytes] = module;
        }
        mModulePages[(module->end - 1 - kIopModuleBase) / kModulePageBytes] = module;
    }
    mMachine.modulePages = mModulePages;
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
    log.info("loaded {} at {:08x}..{:08x}{}", name, base, module.program.imageEnd,
             translated != nullptr ? " translated" : "");
    mLoaded.push_back(module);
    if (translated != nullptr) {
        translated->importBase = static_cast<u32>(mImports.size());
        for (const TranslatedImport& import : translated->imports) {
            mImports.push_back(Import{std::string(import.module), import.id});
        }
        mActive.push_back(translated);
        mMachine.modules = mActive;
        indexModulePages();
    }

    const u32 sequence = mKernel.sequence();
    mKernel.enterHostThread();
    enterModule(mInterpreter.iop, module);
    const Stop stop = enter();
    if (stop.reason != StopReason::Exited || !sync()) {
        log.stub("iop module entry: {}", describe(stop));
        mKernel.dropThreadsAbove(sequence);
        if (translated != nullptr) {
            mActive.pop_back();
            mMachine.modules = mActive;
            indexModulePages();
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

// A tick can land while a call is parked on a coroutine, and resuming it
// again would run one thread twice.
void Iop::advance(u64 instructions) {
    if (mAdvancing) {
        return;
    }
    mAdvancing = true;
    mKernel.advanceTime(instructions);
    mMmio.advance(instructions);
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

    // Nothing else moves the clock while the EE is parked on a call, so a call
    // that waits on a transfer has to be allowed to reach its completion.
    const Stop stop = enter();
    if (stop.reason != StopReason::Exited) {
        log.stub("iop call {} ({:08x}, {:08x}, {:08x}): {}", mSymbols.describe(address),
                 arguments.size() > 0 ? arguments[0] : 0, arguments.size() > 1 ? arguments[1] : 0,
                 arguments.size() > 2 ? arguments[2] : 0, describe(stop));
        reportRegisters();
        mKernel.reportThreads(mSymbols);
        return 0;
    }
    const u32 result = cpu.gpr[kResultRegister];
    sync();
    return result;
}

}  // namespace uee
