#include "iop_run.h"

#include "idle.h"

#include "instruction.h"
#include "log.h"

#include <minicoro.h>

#include <format>
#include <set>
#include <optional>
#include <vector>

namespace uee {
namespace {

LoggerChannel log("ioprun");

constexpr u64 kCallLimit = 1'000'000'000;

// A guest call becomes a host call, so every thread needs its own host stack.
// Bounded by kIopStackSize the same way the EE's is by its guest stack.
constexpr usize kThreadStackSize = 512 * 1024;

struct Step {
    std::optional<Stop> stop;
    bool hasDelaySlot = false;
    bool tookBranch = false;
};

// The interpreter commits to the next pc before it knows the op can run.
Step stopAt(R3000A& cpu, Stop stop) {
    cpu.pc = stop.pc;
    return {std::move(stop)};
}

Step stepOnce(IopMachine& machine, u32 pc) {
    R3000A& cpu = machine.iop().cpu;
    const Instruction insn{machine.memory.read32(pc)};
    if (auto address = machine.memory.takeFault()) {
        return stopAt(cpu, {StopReason::UnmappedAccess, pc,
                            std::format("fetch from {:08x}", *address)});
    }

    if (getOp(insn) == Op::syscall) {
        commitLoad(cpu);
        const u32 index = importIndex(insn.word);
        if (!hostCall(machine, index, pc)) {
            return stopAt(cpu, {StopReason::UnknownSyscall, pc, machine.kernel.nameOf(index)});
        }
        return {};
    }

    switch (machine.interpreter.execute(insn)) {
        case Status::Executed: break;
        case Status::Unimplemented:
            return stopAt(cpu, {StopReason::Unimplemented, pc, std::format("{}", insn)});
        case Status::Undecodable:
            return stopAt(cpu, {StopReason::Undecodable, pc, std::format("{:08x}", insn.word)});
    }

    if (auto address = machine.memory.takeFault()) {
        return stopAt(cpu, {StopReason::UnmappedAccess, pc,
                            std::format("{} touched {:08x}", insn, *address)});
    }
    return {std::nullopt, machine.interpreter.hasDelaySlot(), machine.interpreter.tookBranch()};
}

Step runDelaySlot(IopMachine& machine, u32 pc) {
    R3000A& cpu = machine.iop().cpu;
    const u32 pending = cpu.pc;
    cpu.pc = pc;
    const Step step = stepOnce(machine, pc);
    if (!step.tookBranch) {
        cpu.pc = pending;
    }
    return step;
}

constexpr u64 kIdleInterval = 0xFFFF;

void reportUntranslated(IopMachine& machine, u32 pc) {
    static std::set<u32> reported;
    if (machine.modules.empty() || !reported.insert(pc).second) {
        return;
    }
    for (const TranslatedIrx* module : machine.modules) {
        if (pc - module->base < module->end - module->base) {
            log.warn("no translation for {:08x}, inside {}", pc, module->name);
            return;
        }
    }
    log.warn("no translation for {:08x}, outside every translated module", pc);
}

Stop runUntil(IopMachine& machine, u32 haltPc, u64& executed, const u64& instructionLimit,
              Idle* idle = nullptr) {
    R3000A& cpu = machine.iop().cpu;
    while (executed < instructionLimit) {
        // A nested run halts on its own address; this one is not code either.
        if (cpu.pc == haltPc || cpu.pc == kIopModuleReturn) {
            return Stop{StopReason::Returned, cpu.pc, {}};
        }
        if (cpu.pc == kIopThreadReturn) {
            log.trace("thread {} returned from its entry", machine.kernel.currentThread());
            machine.kernel.exitCurrentThread();
        }

        if (runTranslated(machine, cpu.pc)) {
            if (auto address = machine.memory.takeFault()) {
                return Stop{StopReason::UnmappedAccess, cpu.pc,
                            std::format("touched {:08x}", *address)};
            }
            ++executed;
            continue;
        }

        const u32 pc = cpu.pc;
        reportUntranslated(machine, pc);
        if (idle != nullptr && (executed & kIdleInterval) == 0) {
            idle->sample(cpu.pc);
        }
        const Step step = stepOnce(machine, pc);
        if (step.stop) {
            return *step.stop;
        }
        ++executed;

        if (step.hasDelaySlot) {
            const Step delayed = runDelaySlot(machine, pc + 4);
            if (delayed.stop) {
                return *delayed.stop;
            }
            ++executed;
        }
    }

    return Stop{StopReason::ReachedLimit, cpu.pc, {}};
}

}  // namespace

IopScheduler::IopScheduler(IopMachine& machine, const Symbols& symbols,
                           std::span<const u32> watched)
    : mMachine(machine), mIdle("iop", symbols, machine.memory, watched) {}

IopScheduler::~IopScheduler() {
    for (mco_coro* coroutine : mCoroutines) {
        if (coroutine != nullptr) {
            mco_destroy(coroutine);
        }
    }
}

Stop IopScheduler::run(u64 budget, Skipping skipping) {
    mDeadline = mExecuted + budget;
    mStopped = false;
    for (;;) {
        if (mMachine.kernel.isIdle()) {
            if (skipping == Skipping::Blocked) {
                return Stop{StopReason::Deadlocked, mMachine.iop().cpu.pc, {}};
            }
            // Nothing else wakes a delayed thread, so the clock moves to the
            // first deadline rather than the run ending on it.
            const std::optional<u64> wake = mMachine.kernel.nextDelay();
            if (!wake) {
                return Stop{StopReason::Deadlocked, mMachine.iop().cpu.pc, {}};
            }
            mMachine.kernel.advanceTimeTo(*wake);
            continue;
        }
        if (mExecuted >= mDeadline) {
            return Stop{StopReason::ReachedLimit, mMachine.iop().cpu.pc, {}};
        }
        mMachine.kernel.switchToRunnable();
        mco_coro* coroutine = coroutineFor(mMachine.kernel.currentThread());
        if (mco_resume(coroutine) != MCO_SUCCESS) {
            return Stop{StopReason::Deadlocked, mMachine.iop().cpu.pc, "coroutine failed"};
        }
        if (mStopped) {
            return mStop;
        }
    }
}

Stop IopScheduler::runNested(u32 haltPc) {
    for (;;) {
        const Stop stop = runUntil(mMachine, haltPc, mExecuted, mDeadline, &mIdle);
        if (stop.reason != StopReason::ReachedLimit) {
            return stop;
        }
        mco_yield(mco_running());
    }
}

void IopScheduler::enter(mco_coro* coroutine) {
    static_cast<IopScheduler*>(mco_get_user_data(coroutine))->runThread();
}

// Returning here would end the coroutine; yielding keeps it alive to reuse.
void IopScheduler::runThread() {
    for (;;) {
        mStop = runUntil(mMachine, kIopModuleReturn, mExecuted, mDeadline, &mIdle);
        mStopped = true;
        mco_yield(mco_running());
    }
}

mco_coro* IopScheduler::coroutineFor(usize thread) {
    if (thread >= mCoroutines.size()) {
        mCoroutines.resize(thread + 1, nullptr);
    }
    if (mCoroutines[thread] == nullptr) {
        mco_desc description = mco_desc_init(enter, kThreadStackSize);
        description.user_data = this;
        mco_create(&mCoroutines[thread], &description);
    }
    return mCoroutines[thread];
}

void enterModule(IopContext& iop, const IrxModule& module, const ModuleArguments& arguments) {
    iop.cpu.pc = module.program.entry;
    iop.cpu.gpr[reg::gp] = module.gp;
    iop.cpu.gpr[reg::sp] = arguments.stackTop;
    iop.cpu.gpr[reg::ra] = kIopModuleReturn;
    // A negative argc unloads rather than starts.
    // CRI_ADXI.IRX md5 df850012de771767b5cd88b6263a25c1: module_start @ 0x57c
    iop.cpu.gpr[reg::a0] = arguments.count;
    iop.cpu.gpr[reg::a1] = arguments.vector;
}

u32 callGuest(IopMachine& machine, u32 function, u32 gp, std::span<const u32> arguments) {
    // Room below the caller's frame for the four argument slots the callee owns.
    constexpr u32 kArgumentSlots = 16;
    R3000A& cpu = machine.iop().cpu;
    const R3000A caller = cpu;
    cpu.gpr[reg::sp] = (cpu.gpr[reg::sp] - kArgumentSlots) & ~7u;
    for (usize i = 0; i < arguments.size() && i < 4; ++i) {
        cpu.gpr[reg::a0 + i] = arguments[i];
    }
    cpu.gpr[reg::gp] = gp;
    cpu.gpr[reg::ra] = kIopNestedReturn;
    cpu.pc = function;
    const Stop stop = machine.scheduler->runNested(kIopNestedReturn);
    const u32 result = cpu.gpr[reg::v0];
    if (stop.reason != StopReason::Returned) {
        log.stub("call to {:08x}: {}", function, describe(stop));
    }
    cpu = caller;
    return result;
}

void waitUntilRunning(IopMachine& machine, usize thread) {
    while (!machine.kernel.isCurrent(thread)) {
        mco_yield(mco_running());
    }
}

// The caller commits the load delay, so $ra predates a load in the call's slot.
bool hostCall(IopMachine& machine, u32 index, u32 pc) {
    R3000A& cpu = machine.iop().cpu;
    const usize self = machine.kernel.currentThread();
    cpu.pc = cpu.gpr[reg::ra];
    if (!machine.kernel.call(index)) {
        cpu.pc = pc;
        return false;
    }
    while (!machine.kernel.isCurrent(self)) {
        mco_yield(mco_running());
    }
    return true;
}

// The same as hostCall, except the service was resolved when the code was
// written out, so nothing here searches for it.
bool hostService(IopMachine& machine, u32 service, u32 pc) {
    R3000A& cpu = machine.iop().cpu;
    const usize self = machine.kernel.currentThread();
    cpu.pc = cpu.gpr[reg::ra];
    machine.kernel.callService(service);
    while (!machine.kernel.isCurrent(self)) {
        mco_yield(mco_running());
    }
    (void)pc;
    return true;
}

void interpreterFallbackDelaySlot(IopMachine& machine, u32 pc) {
    runDelaySlot(machine, pc);
}

bool runTranslated(IopMachine& machine, u32 pc) {
    // Modules are packed without regard to pages, so two can share one and the
    // hint is only a hint. The walk below is what answers when it misses.
    const u32 page = (pc - kIopModuleBase) / kModulePageBytes;
    if (page < machine.modulePages.size()) {
        TranslatedIrx* const module = machine.modulePages[page];
        if (module != nullptr && pc - module->base < module->end - module->base) {
            return module->run(machine, pc);
        }
    }
    for (const TranslatedIrx* module : machine.modules) {
        if (pc - module->base < module->end - module->base) {
            return module->run(machine, pc);
        }
    }
    return false;
}

void interpreterFallback(IopMachine& machine, u32 pc) {
    machine.iop().cpu.pc = pc;
    // Dispatching here would run a whole call tree where one instruction was asked for.
    IopMachine interpretOnly{machine.interpreter, machine.memory, machine.kernel, {}};
    u64 executed = 0;
    runUntil(interpretOnly, pc + 4, executed, kCallLimit);
}

bool call(IopMachine& machine, u32 target, u32 returnTo) {
    machine.iop().cpu.pc = target;
    if (!runTranslated(machine, target)) {
        reportUntranslated(machine, target);
        // Dispatching inside the subtree would put host frames at a depth the
        // guest stack does not have.
        IopMachine interpretOnly{machine.interpreter, machine.memory, machine.kernel, {}};
        u64 executed = 0;
        runUntil(interpretOnly, returnTo, executed, kCallLimit);
    }
    return machine.iop().cpu.pc == returnTo;
}

bool stopAtIop(IopMachine& machine, u32 blockStart) {
    machine.iop().cpu.pc = blockStart;
    return true;
}

}  // namespace uee
