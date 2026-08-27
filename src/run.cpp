#include "run.h"

#include "instruction.h"
#include "translate.h"

#include <minicoro.h>

#include <format>
#include <optional>
#include <vector>

namespace uee {
namespace {

constexpr u32 kArgumentRegister = 4;
constexpr u32 kReturnAddressRegister = 31;

constexpr u32 kCallbackReturn = 0xF0000000;
// The guest program counter is always aligned, so an odd address never matches.
constexpr u32 kNoHalt = 1;

// Guest calls become host calls, so this carries the guest's own call depth.
constexpr usize kThreadStackSize = 64 * 1024 * 1024;

constexpr u64 kCallLimit = 1'000'000'000;

struct Step {
    std::optional<Stop> stop;
    bool hasDelaySlot = false;
};

Step stepOnce(Interpreter& interpreter, Memory& memory, Kernel& kernel, u32 pc) {
    const Instruction insn{memory.read32(pc)};
    if (auto address = memory.fault()) {
        return {Stop{StopReason::UnmappedAccess, pc, std::format("fetch from {:08x}", *address)}};
    }

    if (opOf(insn) == Op::syscall) {
        interpreter.ee.cpu.pc = pc + 4;
        switch (kernel.handleSyscall()) {
            case SyscallResult::Handled: return {};
            case SyscallResult::Exited: return {Stop{StopReason::Exited, pc, {}}};
            case SyscallResult::Deadlocked: return {Stop{StopReason::Deadlocked, pc, {}}};
            case SyscallResult::Unknown:
                return {Stop{StopReason::UnknownSyscall, pc,
                             std::format("{}", static_cast<s32>(
                                                   interpreter.ee.cpu.gpr[3].lo))}};
        }
    }

    switch (interpreter.execute(insn)) {
        case Status::Executed: break;
        case Status::Unimplemented:
            return {Stop{StopReason::Unimplemented, pc, std::format("{}", insn)}};
        case Status::Undecodable:
            return {Stop{StopReason::Undecodable, pc, std::format("{:08x}", insn.word)}};
    }

    if (auto address = memory.fault()) {
        return {Stop{StopReason::UnmappedAccess, pc,
                     std::format("{} touched {:08x}", insn, *address)}};
    }
    return {std::nullopt, interpreter.hasDelaySlot()};
}

Stop runUntil(Machine& machine, u32 haltPc, u64& executed, u64 instructionLimit);

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
        mStop = runUntil(mMachine, kNoHalt, mExecuted, mInstructionLimit);
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

Stop runUntil(Machine& machine, u32 haltPc, u64& executed, u64 instructionLimit) {
    Interpreter& interpreter = machine.interpreter;
    Memory& memory = machine.memory;
    Kernel& kernel = machine.kernel;
    R5900& cpu = interpreter.ee.cpu;

    while (executed < instructionLimit) {
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
            const u32 resume = cpu.pc;
            cpu.pc = pc + 4;
            const Step delayed = stepOnce(interpreter, memory, kernel, pc + 4);
            if (delayed.stop) {
                return *delayed.stop;
            }
            ++executed;
            cpu.pc = resume;
        }

        if (auto call = kernel.takePendingCall()) {
            const EeContext caller = interpreter.ee;
            cpu.pc = call->function;
            cpu.write64(kArgumentRegister, call->argument);
            cpu.write64(kReturnAddressRegister, kCallbackReturn);

            const Stop stop = runUntil(machine, kCallbackReturn, executed, instructionLimit);
            if (stop.reason != StopReason::Returned) {
                return stop;
            }
            interpreter.ee = caller;
        }
    }

    return Stop{StopReason::ReachedLimit, cpu.pc, {}};
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

void step(Machine& machine, u32 pc) {
    machine.ee().cpu.pc = pc;
    // Without this the pc could land on a function entry and re-enter translated
    // code, running a whole call tree where one instruction was asked for.
    Machine interpretOnly{machine.interpreter, machine.memory, machine.kernel, nullptr};
    u64 executed = 0;
    // A syscall can queue a guest callback that has to run before the caller
    // carries on, and that callback needs a budget of its own.
    runUntil(interpretOnly, pc + 4, executed, kCallLimit);
}

bool call(Machine& machine, u32 target, u32 returnTo) {
    machine.ee().cpu.pc = target;
    if (machine.functions == nullptr || !machine.functions(machine, target)) {
        // Dispatching inside the subtree would put host frames at a depth
        // the guest stack does not have.
        Machine interpretOnly{machine.interpreter, machine.memory, machine.kernel, nullptr};
        u64 executed = 0;
        runUntil(interpretOnly, returnTo, executed, kCallLimit);
    }
    return machine.ee().cpu.pc == returnTo;
}

Stop run(Machine& machine, u64 instructionLimit) {
    return Scheduler(machine, instructionLimit).run();
}

}  // namespace uee
