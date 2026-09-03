#include "log.h"
#include "run.h"

#include <set>

#include "instruction.h"
#include "timers.h"
#include "handlers.h"
#include "ee_translate.h"
#include "idle.h"

#include <minicoro.h>

#include <chrono>
#include <format>
#include <optional>
#include <vector>

namespace uee {

// Wall clock: an instruction count measures this interpreter, not the guest.
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

namespace {

LoggerChannel log("ee");

constexpr u32 kArgumentRegister = 4;
constexpr u32 kResultRegister = 2;
constexpr u32 kReturnAddressRegister = 31;

constexpr u32 kCallbackReturn = 0xF0000000;
// The guest program counter is always aligned, so an odd address never matches.
constexpr u32 kNoHalt = 1;

// A host frame per guest call, so this is bounded by the guest stack. Filling
// a 128 KiB guest stack with the smallest frame reaches 15,359 calls, 256 KiB.
constexpr usize kThreadStackSize = 1024 * 1024;

// Only long enough that a real syscall never trips it.
constexpr std::chrono::seconds kCallLimit{30};

constexpr u64 kIdleStep = kInstructionsPerHsync;

// A display wait is a field or two, so longer is nothing coming.
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

    if (getOp(insn) == Op::syscall) {
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

// Nothing readies a thread until a handler returns, so do not idle under one.
enum class Idling { Allowed, Blocked };

// A nested run puts its own limit in front of the one outside it.
class DeadlineScope {
public:
    DeadlineScope(Machine& machine, Deadline& deadline)
        : mMachine(machine), mPrevious(std::exchange(machine.deadline, &deadline)) {}
    ~DeadlineScope() { mMachine.deadline = mPrevious; }

private:
    Machine& mMachine;
    Deadline* mPrevious;
};

// Only the outermost run watches: a callback is bounded by returning.
constexpr u64 kIdleInterval = 0xFFFF;

// The thread cannot progress on the stack it is parked on.
constexpr u64 kIdleResumesReported = 100000;

// Without this a run that slows to a crawl reads as a hang.
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

// A thread parked mid call tree resumes there, so its host stack is kept.
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

// The R5900 lets the second transfer win rather than queueing both.
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

    const DeadlineScope limit(machine, deadline);

    while (!deadline.passed() && !machine.budget.stopped) {
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

        // Exact here; the interval only spares emitted code, which reports a block.
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
        if (machine.hooks != nullptr && machine.hooks->isHooked(pc) &&
            machine.hooks->enter(interpreter.ee)) {
            continue;
        }
        if (!kernel.symbolWatch().empty()) {
            kernel.symbolWatch().enter(memory, pc, static_cast<u32>(cpu.gpr[4].lo),
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

        // Chaining nested calls would finish a handler chain in reverse order.
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
            // Nothing else picks up a thread the handler readied.
            kernel.dispatchAfterInterrupt();
            // A thread with its own host stack must not be run from here.
            while (kernel.currentThread() != self) {
                mco_yield(mco_running());
            }
        }
    }

    return Stop{StopReason::TimedOut, cpu.pc, {}};
}

// A stop here is the callback's own, not a deadlock.
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

// Out of line: the callback it may run needs a frame every block would pay for.
void addPerformanceEvents(R5900& cpu, u32 instructions) {
    for (u32 i = 0; i < 2; ++i) {
        const u32 shift = 10u * i;
        const bool counting = ((cpu.performanceControl >> (1 + shift)) & 0xFu) != 0;
        const u32 event = (cpu.performanceControl >> (5 + shift)) & 0x1Fu;
        if (counting && event == cop0::kProcessorCycleEvent) {
            cpu.performance[i] += instructions;
        }
    }
}

bool stopAt(Machine& machine, u32 blockStart) {
    machine.ee().cpu.pc = blockStart;
    return false;
}

bool service(Machine& machine, u32 instructions) {
    Kernel& kernel = machine.kernel;
    if (machine.budget.executed >= machine.budget.limit ||
        (machine.deadline != nullptr && machine.deadline->passed())) {
        machine.budget.stopped = true;
        return false;
    }
    countPerformanceEvents(machine.ee().cpu,
                           static_cast<u32>(machine.budget.executed - machine.budget.counted));
    machine.budget.counted = machine.budget.executed;
    (void)instructions;
    machine.budget.nextService = machine.budget.executed + Budget::kServiceInterval;
    if (kernel.inHandler()) {
        return true;
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
    return true;
}

// The pc is left on the syscall so the driver re-runs it and reports the stop.
bool syscall(Machine& machine, s32 number, u32 resumePc) {
    Kernel& kernel = machine.kernel;
    const usize self = kernel.currentThread();

    machine.ee().cpu.pc = resumePc;
    if (kernel.callSyscall(number) != SyscallResult::Handled) {
        machine.ee().cpu.pc = resumePc - 4;
        return false;
    }

    // Every thread looks blocked while a handler is on the stack, and idling
    // there would unwind it.
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

void interpreterFallbackDelaySlot(Machine& machine, u32 pc) {
    runDelaySlot(machine, pc);
}

void interpreterFallback(Machine& machine, u32 pc) {
    machine.ee().cpu.pc = pc;
    // Without this the pc could land on a function entry and re-enter translated
    // code, running a whole call tree, while only one instruction was asked for.
    Machine interpretOnly{machine.interpreter, machine.memory, machine.mmio, machine.kernel,
                          machine.budget, nullptr};
    Deadline deadline{kCallLimit};
    runUntil(interpretOnly, pc + 4, deadline,
             machine.kernel.inHandler() ? Idling::Blocked : Idling::Allowed);
}

bool askFrontend(Machine& machine, u32 pc) {
    machine.ee().cpu.pc = pc;
    return machine.hooks->enter(machine.ee());
}

// The whole subtree below it runs a hundred times slower, so say so once.
void reportUntranslated(Machine& machine, u32 target) {
    static std::set<u32> reported;
    if (!reported.insert(target).second) {
        return;
    }
    log.warn("no translation for {}, interpreting it and everything it calls",
             machine.kernel.symbols().describe(target));
}

bool call(Machine& machine, u32 target, u32 returnTo) {
    machine.ee().cpu.pc = target;
    if (machine.functions == nullptr || !machine.functions(machine, target)) {
        if (machine.functions != nullptr) {
            reportUntranslated(machine, target);
        }
        // Dispatching inside the subtree would put host frames at a depth
        // the guest stack does not have.
        Machine interpretOnly{machine.interpreter, machine.memory, machine.mmio, machine.kernel,
                          machine.budget, nullptr};
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
