#include "iop_run.h"

#include "instruction.h"

#include <minicoro.h>

#include <format>
#include <optional>
#include <vector>

namespace uee {
namespace {

constexpr u64 kCallLimit = 1'000'000'000;

// A guest call becomes a host call, so every thread needs its own host stack.
constexpr usize kThreadStackSize = 8 * 1024 * 1024;

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
    if (auto address = machine.memory.fault()) {
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

    if (auto address = machine.memory.fault()) {
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

Stop runUntil(IopMachine& machine, u32 haltPc, u64& executed, u64 instructionLimit) {
    R3000A& cpu = machine.iop().cpu;

    while (executed < instructionLimit) {
        if (cpu.pc == haltPc) {
            return Stop{StopReason::Returned, cpu.pc, {}};
        }
        if (cpu.pc == kIopThreadReturn) {
            machine.kernel.exitCurrentThread();
        }

        if (machine.functions != nullptr && machine.functions(machine, cpu.pc)) {
            if (auto address = machine.memory.fault()) {
                return Stop{StopReason::UnmappedAccess, cpu.pc,
                            std::format("touched {:08x}", *address)};
            }
            ++executed;
            continue;
        }

        const u32 pc = cpu.pc;
        const usize self = machine.kernel.currentThread();
        const Step step = stepOnce(machine, pc);
        if (step.stop) {
            return *step.stop;
        }
        ++executed;
        // The call gave the processor away; wait until it comes back.
        while (machine.kernel.currentThread() != self) {
            mco_yield(mco_running());
        }

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

// One coroutine per guest thread, so a thread parked in a call tree resumes
// where it left off.
class Scheduler {
public:
    Scheduler(IopMachine& machine, u64 instructionLimit)
        : mMachine(machine), mInstructionLimit(instructionLimit) {}

    ~Scheduler() {
        for (mco_coro* coroutine : mCoroutines) {
            if (coroutine != nullptr) {
                mco_destroy(coroutine);
            }
        }
    }

    Stop run() {
        for (;;) {
            if (mMachine.kernel.isIdle()) {
                return Stop{StopReason::Deadlocked, mMachine.iop().cpu.pc, {}};
            }
            mco_coro* coroutine = coroutineFor(mMachine.kernel.currentThread());
            if (mco_status(coroutine) == MCO_DEAD) {
                return mStop;
            }
            if (mco_resume(coroutine) != MCO_SUCCESS) {
                return Stop{StopReason::Deadlocked, mMachine.iop().cpu.pc, "coroutine failed"};
            }
            if (mStopped) {
                return mStop;
            }
        }
    }

private:
    static void enter(mco_coro* coroutine) {
        static_cast<Scheduler*>(mco_get_user_data(coroutine))->runThread();
    }

    void runThread() {
        mStop = runUntil(mMachine, kIopModuleReturn, mExecuted, mInstructionLimit);
        mStopped = true;
    }

    mco_coro* coroutineFor(usize thread) {
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

    IopMachine& mMachine;
    u64 mInstructionLimit;
    std::vector<mco_coro*> mCoroutines;
    u64 mExecuted = 0;
    Stop mStop;
    bool mStopped = false;
};

}  // namespace

void enterModule(IopContext& iop, const IrxModule& module) {
    iop.cpu.pc = module.program.entry;
    iop.cpu.gpr[reg::gp] = module.gp;
    iop.cpu.gpr[reg::sp] = kIopStackTop;
    iop.cpu.gpr[reg::ra] = kIopModuleReturn;
}

// The caller commits the load delay, so $ra is still the value from before a
// load in the call's own delay slot.
bool hostCall(IopMachine& machine, u32 index, u32 pc) {
    R3000A& cpu = machine.iop().cpu;
    cpu.pc = cpu.gpr[reg::ra];
    if (!machine.kernel.call(index)) {
        cpu.pc = pc;
        return false;
    }
    return true;
}

void stepDelaySlot(IopMachine& machine, u32 pc) {
    runDelaySlot(machine, pc);
}

void step(IopMachine& machine, u32 pc) {
    machine.iop().cpu.pc = pc;
    // Dispatching here would run a whole call tree where one instruction was asked for.
    IopMachine interpretOnly{machine.interpreter, machine.memory, machine.kernel, nullptr};
    u64 executed = 0;
    runUntil(interpretOnly, pc + 4, executed, kCallLimit);
}

bool call(IopMachine& machine, u32 target, u32 returnTo) {
    machine.iop().cpu.pc = target;
    if (machine.functions == nullptr || !machine.functions(machine, target)) {
        // Dispatching inside the subtree would put host frames at a depth the
        // guest stack does not have.
        IopMachine interpretOnly{machine.interpreter, machine.memory, machine.kernel, nullptr};
        u64 executed = 0;
        runUntil(interpretOnly, returnTo, executed, kCallLimit);
    }
    return machine.iop().cpu.pc == returnTo;
}

Stop run(IopMachine& machine, u64 instructionLimit) {
    Stop stop = Scheduler(machine, instructionLimit).run();
    if (stop.reason == StopReason::Returned) {
        stop.reason = StopReason::Exited;
    }
    return stop;
}

}  // namespace uee
