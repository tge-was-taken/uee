#include "log.h"
#include "run.h"


#include "instruction.h"
#include "timers.h"
#include "handlers.h"
#include "translate.h"
#include "idle.h"

#include <minicoro.h>

#include <chrono>
#include <format>
#include <optional>
#include <vector>

namespace uee {
namespace {

LoggerChannel log("ee");

constexpr u32 kArgumentRegister = 4;
constexpr u32 kResultRegister = 2;
constexpr u32 kReturnAddressRegister = 31;

constexpr u32 kCallbackReturn = 0xF0000000;
// The guest program counter is always aligned, so an odd address never matches.
constexpr u32 kNoHalt = 1;

// Guest calls become host calls, so this carries the guest's own call depth.
constexpr usize kThreadStackSize = 8 * 1024 * 1024;

// A syscall the guest is inside cannot outlast the run that made it, so this
// only has to be long enough that a real one never trips it.
constexpr std::chrono::seconds kCallLimit{30};

constexpr u64 kIdleStep = kInstructionsPerHsync;

// A guest waiting on the display is idle for a field or two, so longer than this
// is nothing coming rather than a run that ended mid wait.
constexpr u64 kIdleFieldsStuck = 60;

struct Step {
    std::optional<Stop> stop;
    bool hasDelaySlot = false;
    bool tookBranch = false;
    bool branchYields = false;
};

// The interpreter commits to the next pc before it knows the op can run.
Step stopAt(R5900& cpu, Stop stop) {
    cpu.pc = stop.pc;
    return {std::move(stop)};
}

Step stepOnce(Interpreter& interpreter, Memory& memory, Kernel& kernel, u32 pc) {
    R5900& cpu = interpreter.ee.cpu;
    const Instruction insn{memory.read32(pc)};
    if (auto address = memory.takeFault()) {
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

    if (auto address = memory.takeFault()) {
        return stopAt(cpu, {StopReason::UnmappedAccess, pc,
                            std::format("{} touched {:08x}", insn, *address)});
    }
    return {std::nullopt, interpreter.hasDelaySlot(), interpreter.tookBranch(),
            interpreter.branchYieldsDelaySlot()};
}

// Nothing can make a thread ready until a handler on the stack returns, so the
// run loop keeps stepping rather than idling under one.
enum class Idling { Allowed, Blocked };

// Only the outermost run watches: a callback is bounded by returning.
constexpr u64 kIdleInterval = 0xFFFF;

// A resume that runs nothing means the thread the kernel points at cannot make
// progress on the stack it is parked on.
constexpr u64 kIdleResumesReported = 100000;

// Wall clock, because how far a guest gets in a given instruction count says
// more about this interpreter's speed than about the guest.
class Deadline {
public:
    explicit Deadline(std::optional<std::chrono::milliseconds> limit)
        : mAt(limit ? std::chrono::steady_clock::now() + *limit
                    : std::chrono::steady_clock::time_point::max()) {}

    bool passed() {
        if (++mSteps < kStride) {
            return mPassed;
        }
        mSteps = 0;
        mPassed = std::chrono::steady_clock::now() >= mAt;
        return mPassed;
    }

private:
    // Asking the clock costs more than the step it bounds.
    static constexpr u32 kStride = 1024;

    std::chrono::steady_clock::time_point mAt;
    u32 mSteps = 0;
    bool mPassed = false;
};

// How fast the guest is actually running. A run that slows to a crawl reads as a
// hang without this, since everything downstream simply stops happening.
class Progress {
public:
    void sample(u64 executed) {
        const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        const std::chrono::duration<double> elapsed = now - mReported;
        if (elapsed < kReportInterval) {
            return;
        }
        log.info("{} million instructions, {:.1f} million a second", executed / 1'000'000,
                 static_cast<double>(executed - mExecuted) / elapsed.count() / 1'000'000);
        mReported = now;
        mExecuted = executed;
    }

private:
    static constexpr std::chrono::seconds kReportInterval{2};

    std::chrono::steady_clock::time_point mReported = std::chrono::steady_clock::now();
    u64 mExecuted = 0;
};

constexpr usize kRunningThread = static_cast<usize>(-1);

Stop runUntil(Machine& machine, u32 haltPc, Deadline& deadline, Idling idling,
              Idle* idle = nullptr,
              Progress* progress = nullptr, usize owner = kRunningThread);
std::optional<Stop> runIdle(Machine& machine, Deadline& deadline);

// A thread that gives up the processor part way through a call tree has to
// resume there, which needs its host stack kept intact.
class Scheduler {
public:
    Scheduler(Machine& machine, std::optional<std::chrono::milliseconds> limit)
        : mMachine(machine), mDeadline(limit) {}

    ~Scheduler() {
        for (mco_coro* coroutine : mCoroutines) {
            if (coroutine != nullptr) {
                mco_destroy(coroutine);
            }
        }
    }

    Stop run() {
        for (;;) {
            if (mDeadline.passed()) {
                return Stop{StopReason::TimedOut, mMachine.ee().cpu.pc, {}};
            }
            mResuming = mMachine.kernel.currentThread();
            mco_coro* coroutine = coroutineFor(mResuming);
            if (mco_status(coroutine) == MCO_DEAD) {
                return mStop;
            }
            const u64 before = mMachine.kernel.executed();
            if (mco_resume(coroutine) != MCO_SUCCESS) {
                return Stop{StopReason::Deadlocked, mMachine.ee().cpu.pc, "coroutine failed"};
            }
            if (mMachine.kernel.executed() != before) {
                mIdleResumes = 0;
            } else if (++mIdleResumes == kIdleResumesReported) {
                log.warn("thread {} was resumed {} times without running an instruction",
                         mMachine.kernel.currentThread(), mIdleResumes);
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
        mStop = runUntil(mMachine, kNoHalt, mDeadline, Idling::Allowed, &mIdle, &mProgress,
                         mResuming);
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
    Deadline mDeadline;
    Idle mIdle{"ee", mMachine.kernel.symbols(), mMachine.memory,
               mMachine.kernel.watched()};
    Progress mProgress;
    std::vector<mco_coro*> mCoroutines;
    Stop mStop;
    bool mStopped = false;
    u64 mIdleResumes = 0;
    usize mResuming = 0;
};

// A slot that transfers control replaces the pending transfer: the R5900 lets
// the second win rather than queueing both.
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

Stop runUntil(Machine& machine, u32 haltPc, Deadline& deadline, Idling idling, Idle* idle,
              Progress* progress, usize owner) {
    Interpreter& interpreter = machine.interpreter;
    Memory& memory = machine.memory;
    Kernel& kernel = machine.kernel;
    R5900& cpu = interpreter.ee.cpu;

    while (!deadline.passed()) {
        if (cpu.pc == kSyscallHandlerReturn && kernel.resumeFromSyscallHandler()) {
            continue;
        }

        if (cpu.pc == haltPc) {
            return Stop{StopReason::Returned, cpu.pc, {}};
        }

        if (machine.functions != nullptr && machine.functions(machine, cpu.pc)) {
            if (auto address = memory.takeFault()) {
                return Stop{StopReason::UnmappedAccess, cpu.pc,
                            std::format("touched {:08x}", *address)};
            }
            continue;
        }

        // One instruction at a time is cheap enough to keep the clock exact here.
        // The interval only spares emitted code, which reports a whole block.
        kernel.countExecuted(1);
        kernel.advanceClock();
        kernel.deliverInterrupts();
        if (machine.stopRequest != nullptr && (kernel.executed() & kIdleInterval) == 0 &&
            machine.stopRequest->isStopRequested()) {
            return Stop{StopReason::Stopped, cpu.pc, {}};
        }
        if (idle != nullptr && (kernel.executed() & kIdleInterval) == 0) {
            progress->sample(kernel.executed());
            if (idle->sample(cpu.pc)) {
                kernel.reportThreads();
            }
        }

        if (kernel.isIdle() && idling == Idling::Allowed) {
            if (const std::optional<Stop> stop = runIdle(machine, deadline)) {
                return *stop;
            }
            continue;
        }

        const u32 pc = cpu.pc;
        if (machine.calls != nullptr && machine.calls->isHooked(pc) &&
            machine.calls->enter(interpreter.ee)) {
            continue;
        }
        if (!kernel.hooks().empty()) {
            kernel.hooks().enter(memory, pc, static_cast<u32>(cpu.gpr[4].lo),
                                 static_cast<u32>(cpu.gpr[5].lo), static_cast<u32>(cpu.gpr[6].lo),
                                 static_cast<u32>(cpu.gpr[7].lo));
        }
        const usize self = owner == kRunningThread ? kernel.currentThread() : owner;
        const Step step = stepOnce(interpreter, memory, kernel, pc);
        if (step.stop) {
            return *step.stop;
        }

        while (kernel.currentThread() != self) {
            mco_yield(mco_running());
        }

        if (step.hasDelaySlot) {
            const Step delayed = runDelaySlot(machine, pc + 4);
            if (delayed.stop) {
                return *delayed.stop;
            }
            kernel.countExecuted(1);
        }

        // A nested run must not start a queued call of its own: chaining them
        // that way would finish a handler chain in the reverse of its order.
        if (idling == Idling::Allowed) if (auto call = kernel.takePendingCall()) {
            log.trace("callback {:08x} for {}", call->function, call->argument);
            const EeContext caller = interpreter.ee;
            cpu.pc = call->function;
            cpu.write64(kArgumentRegister, call->argument);
            cpu.write64(kArgumentRegister + 1, call->secondArgument);
            cpu.write64(kArgumentRegister + 2, call->thirdArgument);
            cpu.write64(kReturnAddressRegister, kCallbackReturn);

            kernel.enterHandler();
            const Stop stop = runUntil(machine, kCallbackReturn, deadline, Idling::Blocked);
            kernel.leaveHandler();
            kernel.completePendingCall(static_cast<u32>(cpu.gpr[kResultRegister].lo));
            if (stop.reason != StopReason::Returned) {
                return stop;
            }
            interpreter.ee = caller;
            // The handler can have readied the thread that gave up the
            // processor to run it, and nothing else will pick that thread up.
            kernel.dispatchAfterInterrupt();
            // That can hand the processor to a thread with a host stack of its
            // own, and this one must not carry on running instructions for it.
            while (kernel.currentThread() != self) {
                mco_yield(mco_running());
            }
        }
    }

    return Stop{StopReason::TimedOut, cpu.pc, {}};
}

// A stop here is the callback's own, not a deadlock; returning nothing means
// the processor has work again.
std::optional<Stop> runIdle(Machine& machine, Deadline& deadline) {
    Kernel& kernel = machine.kernel;
    R5900& cpu = machine.ee().cpu;

    u64 idled = 0;
    while (kernel.isIdle() && !deadline.passed()) {
        kernel.countExecuted(kIdleStep);
        idled += kIdleStep;
        kernel.advanceClock();
        kernel.deliverInterrupts();

        if (const std::optional<GuestCall> call = kernel.takePendingCall()) {
            const EeContext caller = machine.ee();
            cpu.pc = call->function;
            cpu.write64(kArgumentRegister, call->argument);
            cpu.write64(kArgumentRegister + 1, call->secondArgument);
            cpu.write64(kArgumentRegister + 2, call->thirdArgument);
            cpu.write64(kReturnAddressRegister, kCallbackReturn);
            kernel.enterHandler();
            const Stop stop = runUntil(machine, kCallbackReturn, deadline, Idling::Blocked);
            kernel.leaveHandler();
            kernel.completePendingCall(static_cast<u32>(cpu.gpr[kResultRegister].lo));
            machine.ee() = caller;
            if (stop.reason != StopReason::Returned) {
                return stop;
            }
            kernel.dispatchAfterInterrupt();
        }
    }
    if (!kernel.isIdle()) {
        return std::nullopt;
    }
    if (idled < kIdleFieldsStuck * kHsyncPerField * kIdleStep) {
        return Stop{StopReason::TimedOut, cpu.pc, {}};
    }
    kernel.reportNoActiveThreads();
    return Stop{StopReason::WentIdle, cpu.pc, {}};
}

std::string_view nameOf(StopReason reason) {
    switch (reason) {
        case StopReason::Exited: return "exited";
        case StopReason::Returned: return "returned";
        case StopReason::ReachedLimit: return "hit the instruction limit";
        case StopReason::TimedOut: return "ran out of time";
        case StopReason::Unimplemented: return "unimplemented instruction";
        case StopReason::Undecodable: return "undecodable instruction";
        case StopReason::UnknownSyscall: return "unknown syscall";
        case StopReason::UnmappedAccess: return "unmapped access";
        case StopReason::Deadlocked: return "every thread is blocked";
        case StopReason::WentIdle: return "the processor went idle with nothing left to wake it";
        case StopReason::Stopped: return "the host stopped the run";
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

// Emitted code holds the host thread, so the clock only moves from in here.
void countExecuted(Machine& machine, u32 instructions) {
    Kernel& kernel = machine.kernel;
    countPerformanceEvents(machine.ee().cpu, instructions);
    if (!kernel.countExecuted(instructions) || kernel.inHandler()) {
        return;
    }

    const usize self = kernel.currentThread();
    kernel.advanceClock();
    kernel.deliverInterrupts();

    while (const std::optional<GuestCall> call = kernel.takePendingCall()) {
        const EeContext caller = machine.ee();
        R5900& cpu = machine.ee().cpu;
        cpu.pc = call->function;
        cpu.write64(kArgumentRegister, call->argument);
        cpu.write64(kArgumentRegister + 1, call->secondArgument);
        cpu.write64(kArgumentRegister + 2, call->thirdArgument);
        cpu.write64(kReturnAddressRegister, kCallbackReturn);

        log.trace("callback {:08x} for {}", call->function, call->argument);
        kernel.enterHandler();
        Deadline deadline{kCallLimit};
        const Stop stop = runUntil(machine, kCallbackReturn, deadline, Idling::Blocked);
        kernel.leaveHandler();

        kernel.completePendingCall(static_cast<u32>(cpu.gpr[kResultRegister].lo));
        if (stop.reason != StopReason::Returned) {
            break;
        }
        machine.ee() = caller;
        kernel.dispatchAfterInterrupt();
    }

    // A handler can have readied a thread with a host stack of its own, and this
    // one must not carry on running instructions for it.
    while (kernel.currentThread() != self) {
        mco_yield(mco_running());
    }
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

    // A handler runs on the context of the thread it interrupted, so every
    // thread looks blocked for as long as it is on the stack. Idling there would
    // report a processor with nothing to run and unwind the handler.
    const Idling idling = kernel.inHandler() ? Idling::Blocked : Idling::Allowed;
    Deadline deadline{kCallLimit};
    while (kernel.isIdle() && idling == Idling::Allowed) {
        if (runIdle(machine, deadline)) {
            return false;
        }
    }
    while (kernel.currentThread() != self) {
        mco_yield(mco_running());
    }
    if (machine.ee().cpu.pc != resumePc) {
        runUntil(machine, resumePc, deadline, idling);
    }
    return true;
}

void stepDelaySlot(Machine& machine, u32 pc) {
    runDelaySlot(machine, pc);
}

void step(Machine& machine, u32 pc) {
    machine.ee().cpu.pc = pc;
    // Without this the pc could land on a function entry and re-enter translated
    // code, running a whole call tree, while only one instruction was asked for.
    Machine interpretOnly{machine.interpreter, machine.memory, machine.kernel, nullptr};
    Deadline deadline{kCallLimit};
    runUntil(interpretOnly, pc + 4, deadline,
             machine.kernel.inHandler() ? Idling::Blocked : Idling::Allowed);
}

bool call(Machine& machine, u32 target, u32 returnTo) {
    machine.ee().cpu.pc = target;
    if (machine.functions == nullptr || !machine.functions(machine, target)) {
        // Dispatching inside the subtree would put host frames at a depth
        // the guest stack does not have.
        Machine interpretOnly{machine.interpreter, machine.memory, machine.kernel, nullptr};
        Deadline deadline{kCallLimit};
        runUntil(interpretOnly, returnTo, deadline,
                 machine.kernel.inHandler() ? Idling::Blocked : Idling::Allowed);
    }
    return machine.ee().cpu.pc == returnTo;
}

Stop run(Machine& machine, std::optional<std::chrono::milliseconds> limit) {
    return Scheduler(machine, limit).run();
}

}  // namespace uee
