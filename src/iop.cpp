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
// A call runs in slices, after each of which the clock moves and an interrupt
// that fell due runs, so a driver that spins on one sees it arrive.
constexpr u64 kCallSlice = 10'000;
constexpr u32 kFirstArgumentRegister = 4;

}  // namespace

Iop::Iop(Memory& mainMemory, std::string& console, std::span<TranslatedIrx* const> translated,
         std::span<const std::string> hostModules, std::span<const u32> watched)
    : mKernel(mInterpreter.iop, mMemory, mainMemory, mImports,
              {kIopHeapBase, kIopStackTop - kIopStackSize}, console),
      mWatched(watched.begin(), watched.end()), mTranslated(translated),
      mHostModules(hostModules.begin(), hostModules.end()) {
    mMmio.attach(mMemory, mKernel);
    mMachine.scheduler = &mScheduler;
    mKernel.attachMachine(mMachine);
    for (const TranslatedIrx* module : mTranslated) {
        mNextBase = std::max(mNextBase, (module->end + 0xFu) & ~0xFu);
    }
}

// "cdrom0:\\LIB31\\PADMAN.IRX" is configured as padman.
ModuleSource Iop::sourceOf(const std::string& name) const {
    // There is no usb controller here, and mass: is the host's own files, so a
    // driver for the real stick, or a file system over one, could only fail.
    constexpr std::string_view kStorageDrivers[] = {"usbd", "usb_driver", "usbhdfsd", "bdm",
                                                "bdmff", "bdmfs_fatfs", "usbmass_bd"};
    std::string bare = name.substr(name.find_last_of(":\\/") + 1);
    bare = bare.substr(0, bare.find('.'));
    for (char& letter : bare) {
        letter = static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
    }
    for (const std::string_view driver : kStorageDrivers) {
        if (driver == bare) {
            return ModuleSource::Host;
        }
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

// Stops as soon as the call waits on the EE, which is parked on this very call;
// the IOP's other threads could otherwise run on it without end.
Stop Iop::enter(IopScheduler::Skipping skipping) {
    for (u64 executed = 0;;) {
        Stop stop = mScheduler.run(kCallSlice, skipping);
        if (mKernel.isHostWaitingOnEe()) {
            return Stop{StopReason::Deadlocked, stop.pc, "the call waits on an RPC to the EE"};
        }
        if (stop.reason == StopReason::ReachedLimit && executed < kInstructionLimit) {
            executed += kCallSlice;
            mKernel.advanceTime(kCallSlice);
            dispatchPendingHandlers();
            continue;
        }
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

// The IOP's loader passes the module its own name as argv[0], the strings ending
// at top, with the pointers to them below.
ModuleArguments Iop::writeArguments(const std::string& name,
                                    std::span<const std::string> arguments, u32 top) {
    if (arguments.empty()) {
        return {};
    }
    std::vector<std::string> strings{name};
    strings.insert(strings.end(), arguments.begin(), arguments.end());
    u32 at = top;
    std::vector<u32> pointers;
    for (const std::string& text : strings) {
        at -= (static_cast<u32>(text.size()) + 4) & ~3u;
        for (usize i = 0; i < text.size(); ++i) {
            mMemory.write8(at + static_cast<u32>(i), static_cast<u8>(text[i]));
        }
        mMemory.write8(at + static_cast<u32>(text.size()), 0);
        pointers.push_back(at);
    }
    at = (at - static_cast<u32>(pointers.size() + 1) * 4) & ~0xFu;
    for (usize i = 0; i < pointers.size(); ++i) {
        mMemory.write32(at + static_cast<u32>(i) * 4, pointers[i]);
    }
    mMemory.write32(at + static_cast<u32>(pointers.size()) * 4, 0);
    return {static_cast<u32>(pointers.size()), at, at - 0x10};
}

// Nothing when the host answers for the module, so its image is not loaded.
std::optional<IrxModule> Iop::linkModule(const std::string& name, std::span<const u8> image) {
    // Loading it would bind later imports to its exports, not to the host.
    if (sourceOf(name) == ModuleSource::Host) {
        log.trace("{} is answered here, so its image is not loaded", name);
        return std::nullopt;
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
    return module;
}

std::optional<s32> Iop::loadModule(const std::string& name, std::span<const u8> image,
                                   std::span<const std::string> arguments) {
    const std::optional<IrxModule> module = linkModule(name, image);
    if (!module) {
        return 0;
    }
    const u32 sequence = mKernel.sequence();
    mKernel.enterHostThread();
    enterModule(mInterpreter.iop, *module, writeArguments(name, arguments, kIopStackTop));
    const Stop stop = enter();
    // Taken before the module's own threads run and reuse the registers.
    const s32 result = static_cast<s32>(mInterpreter.iop.cpu.gpr[reg::v0]);
    if (stop.reason != StopReason::Exited || !sync()) {
        log.stub("iop module entry: {}", describe(stop));
        mKernel.dropThreadsAbove(sequence);
        if (translationOf(name) != nullptr) {
            mActive.pop_back();
            mMachine.modules = mActive;
            indexModulePages();
        }
        return std::nullopt;
    }
    return result;
}

std::optional<s32> Iop::loadModuleOnThread(const std::string& name, std::span<const u8> image,
                                           std::span<const std::string> arguments) {
    const std::optional<IrxModule> module = linkModule(name, image);
    if (!module) {
        return 0;
    }
    constexpr u32 kArgumentBytes = 0x200;
    ModuleArguments start;
    if (!arguments.empty()) {
        const u32 block = mKernel.allocateMemory(kArgumentBytes);
        start = writeArguments(name, arguments, block + kArgumentBytes);
    }
    const u32 values[2] = {start.count, start.vector};
    return static_cast<s32>(callGuest(mMachine, module->program.entry, module->gp, values));
}

void Iop::addHostServer(u32 id, SifServer& server) {
    if (mRpcLoopEntry == 0) {
        constexpr u32 kRpcLoopOrdinal = 22;
        mImports.push_back(Import{"sifcmd", kRpcLoopOrdinal});
        mRpcLoopEntry = mKernel.allocateMemory(8);
        mMemory.write32(mRpcLoopEntry, importSyscall(static_cast<u32>(mImports.size() - 1)));
        mMemory.write32(mRpcLoopEntry + 4, 0);
    }
    mKernel.addHostServer(id, server, mRpcLoopEntry);
}

void Iop::startVblank() {
    mKernel.raiseVblankStart();
}

void Iop::endVblank() {
    mKernel.raiseVblankEnd();
}

// Runs for free: the EE's clock is what makes IOP time, so a server answers
// without the clock moving, and a handler that sleeps waits for the next ticks.
void Iop::runRequested() {
    if (mAdvancing) {
        return;
    }
    mAdvancing = true;
    mKernel.exitHostThread();
    for (u64 executed = 0; mKernel.hasUnansweredCalls() && executed < kInstructionLimit;) {
        const u64 before = mScheduler.getExecuted();
        const Stop stop = mScheduler.run(kCallSlice, IopScheduler::Skipping::Blocked);
        executed += mScheduler.getExecuted() - before;
        if (dispatchPendingHandlers() || stop.reason == StopReason::ReachedLimit) {
            continue;
        }
        if (stop.reason != StopReason::Deadlocked) {
            log.stub("iop request: {}", describe(stop));
        }
        break;
    }
    mAdvancing = false;
}

// A tick can land while a call is parked on a coroutine, and resuming it
// again would run one thread twice. A module entry runs the clock on by itself
// while the EE is parked, so a tick grants its cycles from wherever that left
// it rather than from a target the devices would then wait out in silence.
void Iop::advance(u64 cycles, u64 instructions) {
    mClockTarget = std::max(mClockTarget, mKernel.getCycles()) + cycles;
    if (mAdvancing) {
        return;
    }
    mAdvancing = true;
    mKernel.advanceTimeTo(mClockTarget);
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

}  // namespace uee
