#include "iop_run.h"

#include "idle.h"

#include "instruction.h"
#include "log.h"

#include <minicoro.h>

#include <format>
#include <optional>
#include <vector>

namespace uee {
namespace {

LoggerChannel log("ioprun");

constexpr u64 kCallLimit = 1'000'000'000;

// A guest call becomes a host call, so every thread needs its own host stack.
constexpr usize kThreadStackSize = 1024 * 1024;

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

    if (opOf(insn) == Op::syscall) {
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
            // Nothing else wakes a delayed thread, so the clock moves to the
            // first deadline rather than the run ending on it.
            const std::optional<u64> wake = mMachine.kernel.nextDelay();
            if (!wake || skipping == Skipping::Blocked) {
                return Stop{StopReason::Deadlocked, mMachine.iop().cpu.pc, {}};
            }
            mMachine.kernel.advanceTimeTo(*wake);
            continue;
        }
        if (mExecuted >= mDeadline) {
            return Stop{StopReason::ReachedLimit, mMachine.iop().cpu.pc, {}};
        }
        mco_coro* coroutine = coroutineFor(mMachine.kernel.currentThread());
        if (mco_resume(coroutine) != MCO_SUCCESS) {
            return Stop{StopReason::Deadlocked, mMachine.iop().cpu.pc, "coroutine failed"};
        }
        if (mStopped) {
            return mStop;
        }
    }
}

void IopScheduler::enter(mco_coro* coroutine) {
    static_cast<IopScheduler*>(mco_get_user_data(coroutine))->runThread();
}

void IopScheduler::runThread() {
    mStop = runUntil(mMachine, kIopModuleReturn, mExecuted, mDeadline, &mIdle);
    mStopped = true;
}

mco_coro* IopScheduler::coroutineFor(usize thread) {
    if (thread >= mCoroutines.size()) {
        mCoroutines.resize(thread + 1, nullptr);
    }
    // Only a thread parked part way through a call keeps its stack. One that
    // ran to the end starts the next run from a fresh one.
    if (mCoroutines[thread] != nullptr && mco_status(mCoroutines[thread]) == MCO_DEAD) {
        mco_destroy(mCoroutines[thread]);
        mCoroutines[thread] = nullptr;
    }
    if (mCoroutines[thread] == nullptr) {
        mco_desc description = mco_desc_init(enter, kThreadStackSize);
        description.user_data = this;
        mco_create(&mCoroutines[thread], &description);
    }
    return mCoroutines[thread];
}

void enterModule(IopContext& iop, const IrxModule& module) {
    iop.cpu.pc = module.program.entry;
    iop.cpu.gpr[reg::gp] = module.gp;
    iop.cpu.gpr[reg::sp] = kIopStackTop;
    iop.cpu.gpr[reg::ra] = kIopModuleReturn;
    // A module entry takes (argc, argv), and a negative argc unloads rather than
    // starts it: CRI_ADXI.IRX md5 df850012de771767b5cd88b6263a25c1 branches on
    // exactly that in module_start @ 0x57c.
    iop.cpu.gpr[reg::a0] = 0;
    iop.cpu.gpr[reg::a1] = 0;
}

// The caller commits the load delay, so $ra is still the value from before a
// load in the call's own delay slot. Every thread operation reaches here, and
// emitted code calls it without passing through the run loop.
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

void stepDelaySlot(IopMachine& machine, u32 pc) {
    runDelaySlot(machine, pc);
}

// One span each, so the module is found before it searches.
bool runTranslated(IopMachine& machine, u32 pc) {
    for (const TranslatedIrx* module : machine.modules) {
        if (pc - module->base < module->end - module->base) {
            return module->run(machine, pc);
        }
    }
    return false;
}

void step(IopMachine& machine, u32 pc) {
    machine.iop().cpu.pc = pc;
    // Dispatching here would run a whole call tree where one instruction was asked for.
    IopMachine interpretOnly{machine.interpreter, machine.memory, machine.kernel, {}};
    u64 executed = 0;
    runUntil(interpretOnly, pc + 4, executed, kCallLimit);
}

bool call(IopMachine& machine, u32 target, u32 returnTo) {
    machine.iop().cpu.pc = target;
    if (!runTranslated(machine, target)) {
        // Dispatching inside the subtree would put host frames at a depth the
        // guest stack does not have.
        IopMachine interpretOnly{machine.interpreter, machine.memory, machine.kernel, {}};
        u64 executed = 0;
        runUntil(interpretOnly, returnTo, executed, kCallLimit);
    }
    return machine.iop().cpu.pc == returnTo;
}

}  // namespace uee
