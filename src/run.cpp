#include "run.h"

#include "instruction.h"
#include "timers.h"
#include "translate.h"

#include <minicoro.h>

#include <format>
#include <optional>
#include <vector>

namespace uee {
namespace {

constexpr u32 kArgumentRegister = 4;
constexpr u32 kResultRegister = 2;
constexpr u32 kReturnAddressRegister = 31;

constexpr u32 kCallbackReturn = 0xF0000000;
// The guest program counter is always aligned, so an odd address never matches.
constexpr u32 kNoHalt = 1;

// Guest calls become host calls, so this carries the guest's own call depth.
constexpr usize kThreadStackSize = 64 * 1024 * 1024;

constexpr u64 kCallLimit = 1'000'000'000;

constexpr u64 kIdleStep = kInstructionsPerHsync;

struct Step {
    std::optional<Stop> stop;
    bool hasDelaySlot = false;
    bool tookBranch = false;
    bool branchYields = false;
};

// A stop leaves the pc on the instruction that caused it. The interpreter
// commits to the next pc before it knows whether it can run the op, so without
// this a caller could not tell a failed instruction from a completed one.
Step stopAt(R5900& cpu, Stop stop) {
    cpu.pc = stop.pc;
    return {std::move(stop)};
}

Step stepOnce(Interpreter& interpreter, Memory& memory, Kernel& kernel, u32 pc) {
    R5900& cpu = interpreter.ee.cpu;
    const Instruction insn{memory.read32(pc)};
    if (auto address = memory.fault()) {
        return stopAt(cpu, {StopReason::UnmappedAccess, pc,
                            std::format("fetch from {:08x}", *address)});
    }

    if (opOf(insn) == Op::syscall) {
        cpu.pc = pc + 4;
        switch (kernel.handleSyscall()) {
            case SyscallResult::Handled: return {};
            case SyscallResult::Exited: return stopAt(cpu, {StopReason::Exited, pc, {}});
            case SyscallResult::Deadlocked: return stopAt(cpu, {StopReason::Deadlocked, pc, {}});
            case SyscallResult::Unknown:
                return stopAt(cpu, {StopReason::UnknownSyscall, pc,
                                    std::format("{}", static_cast<s32>(cpu.gpr[3].lo))});
        }
    }

    switch (interpreter.execute(insn)) {
        case Status::Executed: break;
        case Status::Unimplemented:
            return stopAt(cpu, {StopReason::Unimplemented, pc, std::format("{}", insn)});
        case Status::Undecodable:
            return stopAt(cpu, {StopReason::Undecodable, pc, std::format("{:08x}", insn.word)});
    }

    if (auto address = memory.fault()) {
        return stopAt(cpu, {StopReason::UnmappedAccess, pc,
                            std::format("{} touched {:08x}", insn, *address)});
    }
    return {std::nullopt, interpreter.hasDelaySlot(), interpreter.tookBranch(),
            interpreter.branchYieldsDelaySlot()};
}

// An interrupt handler runs on the context it interrupted, so while one is on
// the stack the run loop must keep stepping rather than idle: nothing can make
// a thread ready until the handler returns.
enum class Idling { Allowed, Blocked };

Stop runUntil(Machine& machine, u32 haltPc, u64& executed, u64 instructionLimit,
              Idling idling);
std::optional<Stop> runIdle(Machine& machine, u64& executed, u64 instructionLimit);

// A thread that gives up the processor part way through a call tree has to
// resume there, which needs its host stack kept intact.
class Scheduler {
public:
    Scheduler(Machine& machine, u64 instructionLimit)
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
            mco_coro* coroutine = coroutineFor(mMachine.kernel.currentThread());
            if (mco_status(coroutine) == MCO_DEAD) {
                return mStop;
            }
            if (mco_resume(coroutine) != MCO_SUCCESS) {
                return Stop{StopReason::Deadlocked, mMachine.ee().cpu.pc, "coroutine failed"};
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
        mStop = runUntil(mMachine, kNoHalt, mExecuted, mInstructionLimit, Idling::Allowed);
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

    Machine& mMachine;
    u64 mInstructionLimit;
    std::vector<mco_coro*> mCoroutines;
    u64 mExecuted = 0;
    Stop mStop;
    bool mStopped = false;
};

// One instruction run as a delay slot, with the pending transfer already in pc.
// A slot that transfers control replaces it: a branch here is not valid MIPS,
// and the R5900 lets the second transfer win rather than queueing both.
Step runDelaySlot(Machine& machine, u32 pc) {
    R5900& cpu = machine.ee().cpu;
    u32 pending = cpu.pc;
    for (u32 slot = pc;; slot += 4) {
        cpu.pc = slot;
        const Step step = stepOnce(machine.interpreter, machine.memory, machine.kernel, slot);
        if (step.stop || !step.tookBranch) {
            if (!step.tookBranch) {
                cpu.pc = pending;
            }
            return step;
        }
        if (!step.branchYields) {
            return step;
        }
        pending = cpu.pc;
    }
}

Stop runUntil(Machine& machine, u32 haltPc, u64& executed, u64 instructionLimit,
              Idling idling) {
    Interpreter& interpreter = machine.interpreter;
    Memory& memory = machine.memory;
    Kernel& kernel = machine.kernel;
    R5900& cpu = interpreter.ee.cpu;

    while (executed < instructionLimit) {
        if (cpu.pc == kSyscallHandlerReturn && kernel.resumeFromSyscallHandler()) {
            continue;
        }

        if (cpu.pc == haltPc) {
            return Stop{StopReason::Returned, cpu.pc, {}};
        }

        if (machine.functions != nullptr && machine.functions(machine, cpu.pc)) {
            if (auto address = memory.fault()) {
                return Stop{StopReason::UnmappedAccess, cpu.pc,
                            std::format("touched {:08x}", *address)};
            }
            ++executed;
            continue;
        }

        kernel.advanceClock(executed);
        kernel.deliverInterrupts();

        if (kernel.isIdle() && idling == Idling::Allowed) {
            if (const std::optional<Stop> stop = runIdle(machine, executed, instructionLimit)) {
                return *stop;
            }
            continue;
        }

        const u32 pc = cpu.pc;
        const usize self = kernel.currentThread();
        const Step step = stepOnce(interpreter, memory, kernel, pc);
        if (step.stop) {
            return *step.stop;
        }
        ++executed;
        while (kernel.currentThread() != self) {
            mco_yield(mco_running());
        }

        if (step.hasDelaySlot) {
            const Step delayed = runDelaySlot(machine, pc + 4);
            if (delayed.stop) {
                return *delayed.stop;
            }
            ++executed;
        }

        // A nested run must not start a queued call of its own: chaining them
        // that way would finish a handler chain in the reverse of its order.
        if (idling == Idling::Allowed) if (auto call = kernel.takePendingCall()) {
            const EeContext caller = interpreter.ee;
            cpu.pc = call->function;
            cpu.write64(kArgumentRegister, call->argument);
            cpu.write64(kArgumentRegister + 1, call->secondArgument);
            cpu.write64(kArgumentRegister + 2, call->thirdArgument);
            cpu.write64(kReturnAddressRegister, kCallbackReturn);

            const Stop stop = runUntil(machine, kCallbackReturn, executed, instructionLimit,
                                       Idling::Blocked);
            kernel.completePendingCall(static_cast<u32>(cpu.gpr[kResultRegister].lo));
            if (stop.reason != StopReason::Returned) {
                return stop;
            }
            interpreter.ee = caller;
            // The handler can have readied the thread that gave up the
            // processor to run it, and nothing else will pick that thread up.
            kernel.dispatchAfterInterrupt();
        }
    }

    return Stop{StopReason::ReachedLimit, cpu.pc, {}};
}

// A stop here is the callback's own, not a deadlock; returning nothing means
// the processor has work again.
std::optional<Stop> runIdle(Machine& machine, u64& executed, u64 instructionLimit) {
    Kernel& kernel = machine.kernel;
    R5900& cpu = machine.ee().cpu;

    while (kernel.isIdle() && executed < instructionLimit) {
        executed += kIdleStep;
        kernel.advanceClock(executed);
        kernel.deliverInterrupts();

        if (const std::optional<GuestCall> call = kernel.takePendingCall()) {
            const EeContext caller = machine.ee();
            cpu.pc = call->function;
            cpu.write64(kArgumentRegister, call->argument);
            cpu.write64(kArgumentRegister + 1, call->secondArgument);
            cpu.write64(kArgumentRegister + 2, call->thirdArgument);
            cpu.write64(kReturnAddressRegister, kCallbackReturn);
            const Stop stop = runUntil(machine, kCallbackReturn, executed, instructionLimit,
                                       Idling::Blocked);
            kernel.completePendingCall(static_cast<u32>(cpu.gpr[kResultRegister].lo));
            machine.ee() = caller;
            if (stop.reason != StopReason::Returned) {
                return stop;
            }
            kernel.dispatchAfterInterrupt();
        }
    }
    if (kernel.isIdle()) {
        kernel.reportNoActiveThreads();
        return Stop{StopReason::WentIdle, cpu.pc, {}};
    }
    return std::nullopt;
}

std::string_view nameOf(StopReason reason) {
    switch (reason) {
        case StopReason::Exited: return "exited";
        case StopReason::Returned: return "returned";
        case StopReason::ReachedLimit: return "hit the instruction limit";
        case StopReason::Unimplemented: return "unimplemented instruction";
        case StopReason::Undecodable: return "undecodable instruction";
        case StopReason::UnknownSyscall: return "unknown syscall";
        case StopReason::UnmappedAccess: return "unmapped access";
        case StopReason::Deadlocked: return "every thread is blocked";
        case StopReason::WentIdle: return "the processor went idle with nothing left to wake it";
    }
    return "unknown";
}

}  // namespace

std::string describe(const Stop& stop) {
    if (stop.detail.empty()) {
        return std::format("{} at {:08x}", nameOf(stop.reason), stop.pc);
    }
    return std::format("{} at {:08x}: {}", nameOf(stop.reason), stop.pc, stop.detail);
}

// False once the guest is not to carry on: the pc is left on the syscall so the
// driver re-runs it and reports the stop, rather than emitted code running past
// an exit into whatever follows it.
bool syscall(Machine& machine, s32 number, u32 resumePc) {
    Kernel& kernel = machine.kernel;
    const usize self = kernel.currentThread();

    machine.ee().cpu.pc = resumePc;
    if (kernel.callSyscall(number) != SyscallResult::Handled) {
        machine.ee().cpu.pc = resumePc - 4;
        return false;
    }

    u64 executed = 0;
    while (kernel.isIdle()) {
        if (runIdle(machine, executed, kCallLimit)) {
            return false;
        }
    }
    while (kernel.currentThread() != self) {
        mco_yield(mco_running());
    }
    if (machine.ee().cpu.pc != resumePc) {
        runUntil(machine, resumePc, executed, kCallLimit, Idling::Allowed);
    }
    return true;
}

void stepDelaySlot(Machine& machine, u32 pc) {
    runDelaySlot(machine, pc);
}

void step(Machine& machine, u32 pc) {
    machine.ee().cpu.pc = pc;
    // Without this the pc could land on a function entry and re-enter translated
    // code, running a whole call tree where one instruction was asked for.
    Machine interpretOnly{machine.interpreter, machine.memory, machine.kernel, nullptr};
    u64 executed = 0;
    // A syscall can queue a guest callback that has to run before the caller
    // carries on, and that callback needs a budget of its own.
    runUntil(interpretOnly, pc + 4, executed, kCallLimit, Idling::Allowed);
}

bool call(Machine& machine, u32 target, u32 returnTo) {
    machine.ee().cpu.pc = target;
    if (machine.functions == nullptr || !machine.functions(machine, target)) {
        // Dispatching inside the subtree would put host frames at a depth
        // the guest stack does not have.
        Machine interpretOnly{machine.interpreter, machine.memory, machine.kernel, nullptr};
        u64 executed = 0;
        runUntil(interpretOnly, returnTo, executed, kCallLimit, Idling::Allowed);
    }
    return machine.ee().cpu.pc == returnTo;
}

Stop run(Machine& machine, u64 instructionLimit) {
    return Scheduler(machine, instructionLimit).run();
}

}  // namespace uee
