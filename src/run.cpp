#include "run.h"

#include "instruction.h"

#include <format>
#include <optional>

namespace uee {
namespace {

constexpr u32 kArgumentRegister = 4;
constexpr u32 kReturnAddressRegister = 31;

constexpr u32 kCallbackReturn = 0xF0000000;
// The guest program counter is always aligned, so an odd address never matches.
constexpr u32 kNoHalt = 1;

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
        interpreter.machine.cpu.pc = pc + 4;
        switch (kernel.handleSyscall()) {
            case SyscallResult::Handled: return {};
            case SyscallResult::Exited: return {Stop{StopReason::Exited, pc, {}}};
            case SyscallResult::Deadlocked: return {Stop{StopReason::Deadlocked, pc, {}}};
            case SyscallResult::Unknown:
                return {Stop{StopReason::UnknownSyscall, pc,
                             std::format("{}", static_cast<s32>(
                                                   interpreter.machine.cpu.gpr[3].lo))}};
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

Stop runUntil(Interpreter& interpreter, Memory& memory, Kernel& kernel, u32 haltPc,
              u64& executed, u64 instructionLimit) {
    R5900& cpu = interpreter.machine.cpu;

    while (executed < instructionLimit) {
        if (cpu.pc == haltPc) {
            return Stop{StopReason::Returned, cpu.pc, {}};
        }

        const u32 pc = cpu.pc;
        const Step step = stepOnce(interpreter, memory, kernel, pc);
        if (step.stop) {
            return *step.stop;
        }
        ++executed;

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
            const EmotionEngine caller = interpreter.machine;
            cpu.pc = call->function;
            cpu.write64(kArgumentRegister, call->argument);
            cpu.write64(kReturnAddressRegister, kCallbackReturn);

            const Stop stop =
                runUntil(interpreter, memory, kernel, kCallbackReturn, executed, instructionLimit);
            if (stop.reason != StopReason::Returned) {
                return stop;
            }
            interpreter.machine = caller;
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

Stop run(Interpreter& interpreter, Memory& memory, Kernel& kernel, u64 instructionLimit) {
    u64 executed = 0;
    return runUntil(interpreter, memory, kernel, kNoHalt, executed, instructionLimit);
}

}  // namespace uee
