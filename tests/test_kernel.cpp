#include "adpcm.h"
#include "alarm.h"
#include "gif.h"
#include "gs.h"
#include "ee_context.h"
#include "interrupts.h"
#include "iop_interpreter.h"
#include "iop_kernel.h"
#include "pad.h"
#include "sio2.h"
#include "spu2.h"
#include "kernel.h"
#include "queue.h"
#include "syscall_table.h"
#include "timers.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <format>
#include <vector>

namespace {

int gFailures = 0;

void check(bool condition, const std::string& what) {
    if (!condition) {
        spdlog::error("failed: {}", what);
        ++gFailures;
    }
}

template <typename T>
void checkEqual(T actual, T expected, const std::string& what) {
    if (actual != expected) {
        spdlog::error("failed: {}: expected {}, got {}", what, expected, actual);
        ++gFailures;
    }
}

struct Node {
    uee::Link link;
    int value = 0;
};

void checkQueueIsFifo() {
    uee::Link head;
    uee::initQueue(head);
    check(uee::isEmpty(head), "a fresh queue is empty");

    std::vector<Node> nodes(3);
    for (int i = 0; i < 3; ++i) {
        nodes[static_cast<usize>(i)].value = i;
        uee::insertHead(head, nodes[static_cast<usize>(i)].link);
    }
    check(!uee::isEmpty(head), "a queue holding nodes is not empty");

    for (int i = 0; i < 3; ++i) {
        uee::Link* node = uee::removeTail(head);
        checkEqual(reinterpret_cast<Node*>(node)->value, i, "removal is oldest first");
    }
    check(uee::isEmpty(head), "the queue empties");
    check(uee::removeTail(head) == nullptr, "an empty queue yields nothing");
}

void checkQueueRotatesOldestToNewest() {
    uee::Link head;
    uee::initQueue(head);
    std::vector<Node> nodes(3);
    for (int i = 0; i < 3; ++i) {
        nodes[static_cast<usize>(i)].value = i;
        uee::insertHead(head, nodes[static_cast<usize>(i)].link);
    }

    uee::rotate(head);
    checkEqual(reinterpret_cast<Node*>(uee::removeTail(head))->value, 1, "rotate drops the oldest");
    checkEqual(reinterpret_cast<Node*>(uee::removeTail(head))->value, 2, "then the next");
    checkEqual(reinterpret_cast<Node*>(uee::removeTail(head))->value, 0,
               "and the rotated one is now newest");
}

void checkAlarmsSortByTarget() {
    uee::AlarmTable alarms;
    const s32 late = alarms.set(300, 0x1000, 0, 0, 0);
    const s32 soon = alarms.set(100, 0x2000, 0, 0, 0);
    const s32 middle = alarms.set(200, 0x3000, 0, 0, 0);
    checkEqual(alarms.count(), 3, "three alarms are active");

    u16 next = 0;
    check(alarms.nextTarget(next), "there is a next target");
    checkEqual<u32>(next, 100, "the soonest alarm is first");

    std::vector<uee::ExpiredAlarm> expired = alarms.takeExpired(250);
    checkEqual(expired.size(), static_cast<usize>(2), "two alarms have expired by tick 250");
    checkEqual(expired[0].id, soon, "the soonest fires first");
    checkEqual(expired[1].id, middle, "then the middle one");
    checkEqual(alarms.count(), 1, "the late alarm is still pending");
    check(alarms.isActive(late), "the late alarm is still allocated");
}

void checkAlarmTargetWraps() {
    uee::AlarmTable alarms;
    // Set at 0xFFF0 for 0x20 ticks, so the target lands at 0x10 having wrapped.
    const s32 wrapped = alarms.set(0x20, 0x1000, 0, 0, 0xFFF0);
    u16 target = 0;
    check(alarms.nextTarget(target), "the wrapped alarm is queued");
    checkEqual<u32>(target, 0x10, "the target wrapped to 0x10");

    check(alarms.takeExpired(0xFFF8).empty(), "it has not expired part way through");
    std::vector<uee::ExpiredAlarm> expired = alarms.takeExpired(0x10);
    checkEqual(expired.size(), static_cast<usize>(1), "it expires once the counter wraps");
    checkEqual(expired[0].id, wrapped, "and it is the alarm that was set");
}

void checkAlarmSortsWrappedAfterPending() {
    uee::AlarmTable alarms;
    const s32 before = alarms.set(0x08, 0x1000, 0, 0, 0xFFF0);  // wraps to 0xFFF8
    const s32 after = alarms.set(0x20, 0x2000, 0, 0, 0xFFF0);   // wraps to 0x10
    u16 target = 0;
    alarms.nextTarget(target);
    checkEqual<u32>(target, 0xFFF8, "an alarm that has not wrapped sorts first");
    check(before != after, "the two alarms got separate slots");
}

void checkAlarmReleaseReportsElapsed() {
    uee::AlarmTable alarms;
    const s32 id = alarms.set(500, 0x1000, 0, 0, 100);
    checkEqual(alarms.release(id, 250), 150, "release reports ticks elapsed since the set");
    checkEqual(alarms.count(), 0, "the released alarm leaves the active list");
    checkEqual(alarms.release(id, 250), -1, "releasing twice fails");

    const s32 wrapped = alarms.set(500, 0x1000, 0, 0, 0xFFF0);
    checkEqual(alarms.release(wrapped, 0x08), 0x18, "elapsed accounts for the counter wrapping");
}

void checkAlarmSlotsRunOut() {
    uee::AlarmTable alarms;
    for (usize i = 0; i < uee::kAlarmCount; ++i) {
        check(alarms.set(100, 0x1000, 0, 0, 0) >= 0, "every slot allocates");
    }
    checkEqual(alarms.set(100, 0x1000, 0, 0, 0), -1, "the sixty-fifth alarm is refused");
}

// Mode bits 0 and 1 pick the clock, and 3 is the one that counts scanlines.
constexpr u32 kCountHsync = 0x83;
constexpr u32 kCountBusClock = 0x80;
constexpr u32 kCountBusClockOver16 = 0x81;
constexpr u32 kCountBusClockOver256 = 0x82;

void checkTimersCountHsync() {
    uee::Timers timers;
    timers.at(3).mode = kCountHsync;
    timers.advanceTo(uee::kInstructionsPerHsync * 5);
    checkEqual<u32>(timers.hsyncCount(), 5, "five ticks after five H-SYNC periods");

    timers.advanceTo(uee::kInstructionsPerHsync * 5 + uee::kInstructionsPerHsync - 1);
    checkEqual<u32>(timers.hsyncCount(), 5, "a partial period does not tick");
}

void checkTimersIgnoreDisabledCounters() {
    uee::Timers timers;
    timers.at(0).mode = 0;
    timers.at(1).mode = kCountHsync;
    timers.advanceTo(uee::kInstructionsPerHsync * 3);
    checkEqual<u32>(timers.at(0).count, 0, "a stopped timer does not count");
    checkEqual<u32>(timers.at(1).count, 3, "a running timer does");
}

// A guest that asks for a bus clock and gets scanlines reads a clock tens of
// times too slow, which looks like a hang rather than a wrong number.
void checkTimerClockSelectSetsTheRate() {
    uee::Timers timers;
    timers.at(0).mode = kCountBusClock;
    timers.at(1).mode = kCountBusClockOver16;
    timers.at(2).mode = kCountBusClockOver256;
    timers.advanceTo(4096);
    checkEqual<u32>(timers.at(0).count, 2048, "the bus clock is half the core clock");
    checkEqual<u32>(timers.at(1).count, 128, "a sixteenth of the bus clock");
    checkEqual<u32>(timers.at(2).count, 8, "a two hundred and fifty sixth of it");
}

// Changing the clock mid-run must not spend the instructions twice.
void checkTimerKeepsItsPlaceAcrossAClockChange() {
    uee::Timers timers;
    timers.at(0).mode = kCountBusClockOver256;
    timers.advanceTo(1024);
    checkEqual<u32>(timers.at(0).count, 2, "two ticks at a period of 512");
    timers.write(uee::kTimerBase + 0x10, kCountBusClock);
    timers.advanceTo(1028);
    checkEqual<u32>(timers.at(0).count, 2 + 2, "the faster clock carries on from there");
}

void checkTimerRegisterWindow() {
    uee::Timers timers;
    check(timers.write(uee::kTimerBase + 3 * uee::kTimerStride + 0x20, 0x1234),
          "T3_COMP accepts a write");
    checkEqual<u32>(timers.at(3).comp, 0x1234, "the compare value lands");
    check(timers.read(uee::kTimerBase + 0x9999) == std::nullopt,
          "an address outside the timer window is refused");
}

void checkInterruptMasksToggle() {
    uee::Interrupts interrupts;
    // INTC_MASK is write-one-to-toggle, so the same write turns a cause off.
    interrupts.write(uee::kIntcMask, 1u << 2);
    check(interrupts.read(uee::kIntcMask).value_or(0) == (1u << 2), "the mask bit sets");
    interrupts.write(uee::kIntcMask, 1u << 2);
    checkEqual<u32>(interrupts.read(uee::kIntcMask).value_or(0), 0u, "and toggles back off");

    interrupts.write(uee::kIntcMask, 1u << 2);
    check(!interrupts.pendingIntc().has_value(), "an unmasked cause alone is not pending");
    interrupts.raiseIntc(2);
    checkEqual<u32>(interrupts.pendingIntc().value_or(99), 2u, "raising it makes it pending");
    interrupts.acknowledgeIntc(2);
    check(!interrupts.pendingIntc().has_value(), "acknowledging clears it");
}

void checkStatusIsWriteOneToClear() {
    uee::Interrupts interrupts;
    interrupts.raiseIntc(4);
    interrupts.write(uee::kIntcStat, 1u << 4);
    check(!interrupts.pendingIntc().has_value(), "writing one to a status bit clears it");
}

void checkDmacMaskLivesInTheUpperHalf() {
    uee::Interrupts interrupts;
    interrupts.raiseDmac(5);
    check(!interrupts.pendingDmac().has_value(), "a masked channel is not pending");
    interrupts.write(uee::kDStat, 0x10000u << 5);
    checkEqual<u32>(interrupts.pendingDmac().value_or(99), 5u, "unmasking makes it pending");
}

// next == -1 inserts at the queue's next end and dispatch starts from prev, so
// those handlers come out in the order they were registered. next == 0 inserts
// at the far end and therefore runs before everything already queued.
void checkHandlerChainOrder() {
    uee::Interrupts interrupts;
    const s32 first = interrupts.addIntcHandler(2, 0x1000, -1, 0, uee::handler_state::kUser);
    const s32 second = interrupts.addIntcHandler(2, 0x2000, -1, 0, uee::handler_state::kUser);
    check(first > 0 && second > 0, "both handlers allocate");

    std::vector<const uee::IntrHandler*> chain = interrupts.intcChain(2);
    checkEqual(chain.size(), static_cast<usize>(2), "the chain holds both");
    checkEqual<u32>(chain[0]->func, 0x1000u, "registration order is kept");
    checkEqual<u32>(chain[1]->func, 0x2000u, "for the second too");

    interrupts.addIntcHandler(2, 0x3000, 0, 0, uee::handler_state::kUser);
    chain = interrupts.intcChain(2);
    checkEqual(chain.size(), static_cast<usize>(3), "the third joins them");
    checkEqual<u32>(chain[0]->func, 0x3000u, "a far-end insert runs first");

    interrupts.removeIntcHandler(2, first);
    chain = interrupts.intcChain(2);
    checkEqual(chain.size(), static_cast<usize>(2), "removal shortens the chain");
}

void checkSbusIsReservedForTheKernel() {
    uee::Interrupts interrupts;
    checkEqual(interrupts.addIntcHandler(1, 0x1000, -1, 0, uee::handler_state::kUser), -1,
               "SBUS refuses a user handler");
    check(interrupts.addIntcHandler(1, 0x1000, -1, 0, uee::handler_state::kKernel) > 0,
          "but accepts a kernel one");
}


// The dispatcher is transcribed from SyscallHandler @ 0x80000280 in KERNEL.elf
// (md5 d3365049f363abad59102ec4d4e2b0af). These pin the four decisions that
// listing makes, in its order.
struct SyscallFixture {
    uee::Memory memory = uee::makeMainMemory();
    uee::EeContext context;
    uee::Kernel kernel{context, memory};

    void enter(s32 number) {
        context.cpu.write64(3, static_cast<u64>(static_cast<s64>(number)));
        context.cpu.pc = 0x00100000;
    }
};

// bltzl $v1, negu $v1: the negated number selects the same slot, and the delay
// slot only runs when the branch is taken, so a positive number is untouched.
void checkNegativeSyscallSelectsTheSameSlot() {
    SyscallFixture positive;
    uee::writeSyscallHandler(positive.memory, 100, 0x00200000);
    positive.enter(100);
    check(positive.kernel.handleSyscall() == uee::SyscallResult::Handled,
          "a positive number reaches its handler");
    checkEqual(positive.context.cpu.pc, 0x00200000u, "and enters it");

    SyscallFixture negative;
    uee::writeSyscallHandler(negative.memory, 100, 0x00200000);
    negative.enter(-100);
    check(negative.kernel.handleSyscall() == uee::SyscallResult::Handled,
          "the negated number reaches the same handler");
    checkEqual(negative.context.cpu.pc, 0x00200000u, "and enters it");
}

// li $k0, 0x7C / bne $k0, $v1 sits after the fall-through, so only a positive
// 124 skips the table.
void checkDeci2BypassesTheTableOnlyWhenPositive() {
    SyscallFixture positive;
    uee::writeSyscallHandler(positive.memory, 124, 0x00200000);
    positive.enter(124);
    check(positive.kernel.handleSyscall() == uee::SyscallResult::Handled,
          "a positive 124 is answered");
    check(positive.context.cpu.pc != 0x00200000u, "without indexing the table");

    SyscallFixture negative;
    uee::writeSyscallHandler(negative.memory, 124, 0x00200000);
    negative.enter(-124);
    negative.kernel.handleSyscall();
    checkEqual(negative.context.cpu.pc, 0x00200000u, "a negative 124 indexes the table");
}

// sll $v1, 2 / lw $k0, k_SyscallTable($v1) with no bound check, which is what
// lets P3FES claim slot 131 for its own FindAddress scanner.
void checkTheTableIndexIsUnchecked() {
    SyscallFixture fixture;
    uee::writeSyscallHandler(fixture.memory, 131, 0x0050d4c0);
    checkEqual(fixture.memory.read32(uee::kSyscallTableBase + 131 * 4), 0x0050d4c0u,
               "slot 131 lands one word past the ROM table");
    fixture.enter(131);
    check(fixture.kernel.handleSyscall() == uee::SyscallResult::Handled, "and dispatches");
    checkEqual(fixture.context.cpu.pc, 0x0050d4c0u, "to the installed handler");
}

// jalr $k0: a guest handler is called and returns with jr $ra, so the caller's
// return address has to survive the call.
void checkGuestHandlersAreCalledNotEntered() {
    SyscallFixture fixture;
    uee::writeSyscallHandler(fixture.memory, 90, 0x0050cc20);
    fixture.context.cpu.write64(31, 0x00104000);
    fixture.enter(90);
    fixture.kernel.handleSyscall();
    checkEqual(fixture.context.cpu.pc, 0x0050cc20u, "the handler is entered by call");
    checkEqual(static_cast<u32>(fixture.context.cpu.gpr[31].lo), uee::kSyscallHandlerReturn,
               "with a return address the run loop recognises");

    fixture.context.cpu.pc = uee::kSyscallHandlerReturn;
    check(fixture.kernel.resumeFromSyscallHandler(), "returning resumes the caller");
    checkEqual(static_cast<u32>(fixture.context.cpu.gpr[31].lo), 0x00104000u,
               "with its own return address restored");
    checkEqual(fixture.context.cpu.pc, 0x00100000u, "at the interrupted pc");
}

// Slot 0 holds UndefinedSyscall in ROM, so an unclaimed number is answered
// rather than unknown.
void checkUnclaimedSlotsAreUndefinedSyscall() {
    SyscallFixture fixture;
    fixture.enter(8);
    check(fixture.kernel.handleSyscall() == uee::SyscallResult::Handled,
          "a ROM-empty slot answers");
}

}  // namespace

// A GIFtag's loop count is in registers, not quadwords, and how many quadwords
// those take depends on the format. Miscounting runs the packet past its end and
// swallows whatever follows, which is silent: no register decodes wrongly, they
// just never arrive.
uee::Gif makeGif(uee::Memory& memory, uee::Gs& gs) {
    uee::Gif gif;
    gif.attach(memory, gs);
    return gif;
}

constexpr u64 tag(u64 loops, u64 format, u64 registers) {
    return loops | (format << 58) | (registers << 60);
}

// The GS register that reports drawing is done, which arrives as an address and
// data pair and so is only seen if the packet before it consumed exactly right.
constexpr u64 kFinishRegister = 0x61;
constexpr u32 kCsrFinish = 1u << 1;
constexpr u64 kAddressData = 0xE;

void feedFinish(uee::Gif& gif, uee::Gs& gs) {
    gif.writeQuadword(tag(1, 0, 1), kAddressData);
    gif.writeQuadword(0, kFinishRegister);
    gs.advanceScanline();
}

void checkRegisterListPacksTwoRegistersPerQuadword() {
    uee::Memory memory = uee::makeMainMemory();
    uee::Gs gs;
    uee::Gif gif = makeGif(memory, gs);

    // Four registers over three loops is twelve registers, so six quadwords.
    gif.writeQuadword(tag(3, 1, 4), 0);
    for (int i = 0; i < 6; ++i) {
        gif.writeQuadword(0, 0);
    }
    feedFinish(gif, gs);
    checkEqual(gs.csr() & kCsrFinish, static_cast<u64>(kCsrFinish),
               "a register list ends after half as many quadwords as registers");
}

void checkPackedSpendsAQuadwordPerRegister() {
    uee::Memory memory = uee::makeMainMemory();
    uee::Gs gs;
    uee::Gif gif = makeGif(memory, gs);

    gif.writeQuadword(tag(3, 0, 2), 0);
    for (int i = 0; i < 6; ++i) {
        gif.writeQuadword(0, 0);
    }
    feedFinish(gif, gs);
    checkEqual(gs.csr() & kCsrFinish, static_cast<u64>(kCsrFinish),
               "a packed packet ends after one quadword per register");
}

void checkImageCountsQuadwordsNotRegisters() {
    uee::Memory memory = uee::makeMainMemory();
    uee::Gs gs;
    uee::Gif gif = makeGif(memory, gs);

    // An image carries pixels, so the register count says nothing about length.
    gif.writeQuadword(tag(4, 2, 3), 0);
    for (int i = 0; i < 4; ++i) {
        gif.writeQuadword(0, 0);
    }
    feedFinish(gif, gs);
    checkEqual(gs.csr() & kCsrFinish, static_cast<u64>(kCsrFinish),
               "an image packet is as many quadwords as it has loops");
}

// A guest clears the bit it is about to wait on after submitting the work, so
// answering before it does that loses the answer.
void checkGsHoldsFinishUntilTheScanlineIsOut() {
    uee::Gs gs;
    gs.raiseFinish();
    checkEqual(gs.csr() & 2u, static_cast<u64>(0), "drawing is not done on submission");
    gs.advanceScanline();
    checkEqual(gs.csr() & 2u, static_cast<u64>(2), "a scanline later it is");
}

// A block of zero nibbles with no filter decodes to silence whatever the shift.
void checkAdpcmDecodesAFlatBlock() {
    std::vector<u8> block(uee::kAdpcmBlockBytes, 0);
    uee::AdpcmState state;
    std::vector<s16> out;
    uee::decodeAdpcm(block, state, out);
    checkEqual(out.size(), uee::kAdpcmBlockSamples, "a block decodes 28 samples");
    check(std::all_of(out.begin(), out.end(), [](s16 s) { return s == 0; }),
          "zero nibbles decode to silence");
}

void checkAdpcmShiftScalesTheNibble() {
    std::vector<u8> block(uee::kAdpcmBlockBytes, 0);
    block[0] = 0x04;
    block[2] = 0x01;
    uee::AdpcmState state;
    std::vector<s16> out;
    uee::decodeAdpcm(block, state, out);
    checkEqual(out[0], static_cast<s16>(1 << 12 >> 4), "the nibble shifts by the block's shift");
}

// Filter 1 carries 60/64 of the previous sample into the next.
void checkAdpcmFilterFollowsThePreviousSamples() {
    std::vector<u8> block(uee::kAdpcmBlockBytes, 0);
    block[0] = 0x10;
    uee::AdpcmState state{1024, 0};
    std::vector<s16> out;
    uee::decodeAdpcm(block, state, out);
    checkEqual(out[0], static_cast<s16>(1024 * 60 / 64), "filter one weights the previous sample");
}

void checkAdpcmCarriesStateAcrossBlocks() {
    std::vector<u8> block(uee::kAdpcmBlockBytes, 0);
    block[0] = 0x04;
    block[2] = 0x01;
    uee::AdpcmState state;
    std::vector<s16> first;
    uee::decodeAdpcm(block, state, first);
    check(state.previous == first.back(), "the state ends on the last sample decoded");
}

void checkAdpcmIgnoresAPartialBlock() {
    std::vector<u8> partial(uee::kAdpcmBlockBytes - 1, 0);
    uee::AdpcmState state;
    std::vector<s16> out;
    uee::decodeAdpcm(partial, state, out);
    check(out.empty(), "a block short of its bytes decodes nothing");
}

// Nothing is pressed unless a check says otherwise, so a frame reads the same
// way every run.
struct TestPad : uee::PadSource {
    u16 held = 0;
    u16 buttons() override { return static_cast<u16>(~held); }
};

std::vector<u8> askPad(uee::DualShock& pad, std::initializer_list<u8> command, usize length) {
    std::vector<u8> frame(command);
    frame.resize(std::max(frame.size(), length), 0);
    std::vector<u8> answer(length, 0);
    pad.answer(frame, answer);
    return answer;
}

void checkPadAnswersAPoll() {
    TestPad source;
    source.held = 0x0800;
    uee::DualShock pad{&source};

    const std::vector<u8> answer = askPad(pad, {0x01, 0x42, 0x00}, 5);
    checkEqual(answer[0], static_cast<u8>(0xFF), "the address byte reads as an idle line");
    checkEqual(answer[1], static_cast<u8>(0x41), "a pad out of config mode answers as digital");
    checkEqual(answer[2], static_cast<u8>(0x5A), "the marker follows the id");
    checkEqual(answer[3], static_cast<u8>(0xF7), "the held button clears its bit");
    checkEqual(answer[4], static_cast<u8>(0xFF), "the buttons nothing holds read as ones");
}

void checkPadReportsConfigMode() {
    TestPad source;
    uee::DualShock pad{&source};

    const std::vector<u8> entering = askPad(pad, {0x01, 0x43, 0x00, 0x01}, 9);
    checkEqual(entering[1], static_cast<u8>(0x41),
               "the frame that asks for config mode still answers as what it was");
    checkEqual(askPad(pad, {0x01, 0x42, 0x00}, 9)[1], static_cast<u8>(0xF3),
               "a pad in config mode answers as config");
    askPad(pad, {0x01, 0x43, 0x00, 0x00}, 9);
    checkEqual(askPad(pad, {0x01, 0x42, 0x00}, 9)[1], static_cast<u8>(0x41),
               "leaving config mode puts the id back");
}

void checkPadSwitchesToAnalog() {
    TestPad source;
    uee::DualShock pad{&source};

    askPad(pad, {0x01, 0x43, 0x00, 0x01}, 9);
    checkEqual(askPad(pad, {0x01, 0x45, 0x00}, 9)[5], static_cast<u8>(0x00),
               "the model says analog is off");
    askPad(pad, {0x01, 0x44, 0x00, 0x01}, 9);
    checkEqual(askPad(pad, {0x01, 0x45, 0x00}, 9)[5], static_cast<u8>(0x01),
               "the model says analog is on");
    askPad(pad, {0x01, 0x43, 0x00, 0x00}, 9);

    const std::vector<u8> answer = askPad(pad, {0x01, 0x42, 0x00}, 9);
    checkEqual(answer[1], static_cast<u8>(0x73), "an analog pad answers with the analog id");
    checkEqual(answer[5], static_cast<u8>(0x7F), "the sticks read centred");
}

void checkPadAnswersTheMappingItReplaces() {
    TestPad source;
    uee::DualShock pad{&source};

    askPad(pad, {0x01, 0x43, 0x00, 0x01}, 9);
    const std::vector<u8> first = askPad(pad, {0x01, 0x4D, 0x00, 0x00, 0x01}, 9);
    checkEqual(first[3], static_cast<u8>(0xFF), "an unmapped motor reads as unmapped");
    const std::vector<u8> second = askPad(pad, {0x01, 0x4D, 0x00, 0xFF, 0xFF}, 9);
    checkEqual(second[3], static_cast<u8>(0x00), "the answer carries the mapping it replaces");
    checkEqual(second[4], static_cast<u8>(0x01), "both motors are answered");
}

// The device raises an interrupt when the queue is answered, which is the only
// reason it needs a kernel at all.
struct Sio2Fixture {
    uee::Memory memory = uee::makeIopMemory();
    uee::Memory mainMemory = uee::makeMainMemory();
    uee::IopInterpreter interpreter{memory};
    std::vector<uee::Import> imports;
    std::string console;
    uee::IopKernel kernel{interpreter.iop,  memory, mainMemory, imports,
                          {uee::kIopHeapBase, 0}, console};
    TestPad source;
    uee::Sio2 device;

    Sio2Fixture() {
        device.attach(kernel);
        device.attachPad(&source);
    }

    void write(u32 address, u32 value) { device.write(address, value); }
    u32 read(u32 address) { return device.read(address).value_or(0); }
};

constexpr u32 kSio2Send3 = 0x00;
constexpr u32 kSio2FifoIn = 0x60;
constexpr u32 kSio2FifoOut = 0x64;
constexpr u32 kSio2Control = 0x68;

void checkSio2AnswersEveryQueuedCommand() {
    Sio2Fixture sio2;
    sio2.write(kSio2Send3, 0 | (5 << 8));
    sio2.write(kSio2Send3 + 4, 1 | (5 << 8));
    sio2.write(kSio2Control, 0x3BC);
    for (const u8 byte : {0x01, 0x42, 0x00, 0x00, 0x00, 0x01, 0x42, 0x00, 0x00, 0x00}) {
        sio2.write(kSio2FifoIn, byte);
    }
    sio2.write(kSio2Control, 0x3BD);

    std::vector<u8> answer;
    for (int i = 0; i < 10; ++i) {
        answer.push_back(static_cast<u8>(sio2.read(kSio2FifoOut)));
    }
    checkEqual(answer[1], static_cast<u8>(0x41), "the port with a pad answers as one");
    checkEqual(answer[6], static_cast<u8>(0xFF),
               "the port with nothing in it answers with an idle line");
}

// Mc2Sio2RunTransfer @ 0x8b8 retries unless bits 12 to 15 of RECV1 read 0x1000,
// and PadStateSlotGet @ 0x4950 gives up on the entry whose bit from 16 is set.
void checkSio2ReportsWhichQueueEntryFoundNothing() {
    constexpr u32 kSio2Receive1 = 0x6C;
    constexpr u32 kTransferBits = 0xF000;
    constexpr u32 kFinished = 0x1000;

    Sio2Fixture withPadOnly;
    withPadOnly.write(kSio2Send3, 0 | (5 << 8));
    withPadOnly.write(kSio2Control, 0x3BC);
    for (const u8 byte : {0x01, 0x42, 0x00, 0x00, 0x00}) {
        withPadOnly.write(kSio2FifoIn, byte);
    }
    withPadOnly.write(kSio2Control, 0x3BD);
    const u32 answered = withPadOnly.read(kSio2Receive1);
    checkEqual(answered & kTransferBits, kFinished, "a queue that found its device reads as done");
    checkEqual((answered >> 16) & 1, 0u, "and leaves its entry unmarked");

    Sio2Fixture withEmptySecond;
    withEmptySecond.write(kSio2Send3, 0 | (5 << 8));
    withEmptySecond.write(kSio2Send3 + 4, 1 | (5 << 8));
    withEmptySecond.write(kSio2Control, 0x3BC);
    for (const u8 byte : {0x01, 0x42, 0x00, 0x00, 0x00, 0x01, 0x42, 0x00, 0x00, 0x00}) {
        withEmptySecond.write(kSio2FifoIn, byte);
    }
    withEmptySecond.write(kSio2Control, 0x3BD);
    const u32 missed = withEmptySecond.read(kSio2Receive1);
    checkEqual((missed >> 16) & 1, 0u, "the entry that found a pad stays unmarked");
    checkEqual((missed >> 17) & 1, 1u, "the entry that found nothing is named");
    checkEqual(missed & kTransferBits, 0xD000u, "and the queue no longer reads as done");
}

// SIO2MAN leaves the FIFO reset bits set once Sio2FifoReset @ 0x1a4 has written
// them, so a level triggered reset would throw away the command that follows.
void checkSio2KeepsTheCommandWrittenAfterAReset() {
    Sio2Fixture sio2;
    sio2.write(kSio2Control, 0x3BC);
    sio2.write(kSio2Send3, 0 | (5 << 8));
    sio2.write(kSio2Control, 0x3BC | 0xC);
    for (const u8 byte : {0x01, 0x42, 0x00, 0x00, 0x00}) {
        sio2.write(kSio2FifoIn, byte);
    }
    sio2.write(kSio2Control, 0x3BC | 0xC | 0x1);

    sio2.read(kSio2FifoOut);
    checkEqual(static_cast<u8>(sio2.read(kSio2FifoOut)), static_cast<u8>(0x41),
               "the command survives the write that starts the queue");
}

// The queue ends at the first empty entry, so a command left over from a longer
// transfer is not answered again.
void checkSio2StopsAtAnEmptyEntry() {
    Sio2Fixture sio2;
    sio2.write(kSio2Send3, 0 | (5 << 8));
    sio2.write(kSio2Send3 + 4, 0);
    sio2.write(kSio2Send3 + 8, 0 | (5 << 8));
    sio2.write(kSio2Control, 0x3BC);
    for (const u8 byte : {0x01, 0x42, 0x00, 0x00, 0x00}) {
        sio2.write(kSio2FifoIn, byte);
    }
    sio2.write(kSio2Control, 0x3BD);

    for (int i = 0; i < 5; ++i) {
        sio2.read(kSio2FifoOut);
    }
    checkEqual(static_cast<u8>(sio2.read(kSio2FifoOut)), static_cast<u8>(0xFF),
               "nothing follows the answer to the one command queued");
}

// The register offsets LIBSD addresses, from its table at 0x4ca0.
constexpr u32 kVoiceVolumeLeft = 0x000;
constexpr u32 kVoiceVolumeRight = 0x002;
constexpr u32 kVoicePitch = 0x004;
constexpr u32 kVoiceAdsr1 = 0x006;
constexpr u32 kVoiceAdsr2 = 0x008;
constexpr u32 kVoiceEnvelope = 0x00A;
constexpr u32 kCoreMixLeft = 0x188;
constexpr u32 kCoreMixRight = 0x190;
constexpr u32 kCoreKeyOn = 0x1A0;
constexpr u32 kCoreTransfer = 0x1A8;
constexpr u32 kVoiceStart = 0x1C0;
constexpr u32 kCoreEndx = 0x340;
constexpr u32 kCoreMasterLeft = 0x760;
constexpr u32 kCoreMasterRight = 0x762;

// A block of one nibble each, which decodes to a constant with the widest
// shift and no filter.
constexpr s16 kFlatSample = 0x1000;
constexpr u32 kBlockAddress = 0x1000;

std::vector<u8> makeFlatBlock(u8 flags) {
    std::vector<u8> block(uee::kAdpcmBlockBytes, 0x11);
    block[0] = 0x00;
    block[1] = flags;
    return block;
}

// Full volume in every stage, and an envelope that rises in three samples and
// then stays there. The decay runs for one sample even at the top sustain
// level, so the slowest decay rate is what keeps it full.
void programVoice(uee::Spu2& spu2, u16 pitch) {
    spu2.writeHalf(kCoreTransfer, 0);
    spu2.writeHalf(kCoreTransfer + 2, kBlockAddress);
    spu2.writeHalf(kVoiceStart, 0);
    spu2.writeHalf(kVoiceStart + 2, kBlockAddress);
    spu2.writeHalf(kVoiceVolumeLeft, 0x3FFF);
    spu2.writeHalf(kVoiceVolumeRight, 0x3FFF);
    spu2.writeHalf(kVoicePitch, pitch);
    spu2.writeHalf(kVoiceAdsr1, 0x00FF);
    spu2.writeHalf(kVoiceAdsr2, 0x1FC0);
    spu2.writeHalf(kCoreMixLeft, 1);
    spu2.writeHalf(kCoreMixRight, 1);
    spu2.writeHalf(kCoreMasterLeft, 0x3FFF);
    spu2.writeHalf(kCoreMasterRight, 0x3FFF);
}

// The envelope, the voice volume and the master volume each scale by a
// fifteen bit fraction, so a full scale chain lands a few units below the
// sample it started from.
void checkLevel(s16 actual, s16 expected, const std::string& what) {
    if (std::abs(actual - expected) > 4) {
        spdlog::error("failed: {}: expected about {}, got {}", what, expected, actual);
        ++gFailures;
    }
}

std::vector<s16> render(uee::Spu2& spu2, usize frames) {
    std::vector<s16> stereo(frames * 2);
    spu2.render(stereo);
    return stereo;
}

void checkSpu2PlaysAKeyedOnVoice() {
    uee::Spu2 spu2;
    const std::vector<u8> block = makeFlatBlock(0);
    programVoice(spu2, 0x1000);
    spu2.receive(0, block);
    spu2.writeHalf(kCoreKeyOn, 1);

    const std::vector<s16> stereo = render(spu2, 8);
    checkEqual(stereo[0] != 0, true, "the voice is heard as soon as it is keyed on");
    checkLevel(stereo[14], kFlatSample, "a flat block plays back at its own level");
    checkLevel(stereo[15], kFlatSample, "both channels carry it");
    check(spu2.readHalf(kVoiceEnvelope).value_or(0) == 0x7FFF,
          "the envelope has reached the top");
}

// The volumes set the panpot, so a voice can be put on one channel alone.
void checkSpu2PansAVoice() {
    uee::Spu2 spu2;
    programVoice(spu2, 0x1000);
    spu2.writeHalf(kVoiceVolumeRight, 0);
    spu2.receive(0, makeFlatBlock(0));
    spu2.writeHalf(kCoreKeyOn, 1);

    const std::vector<s16> stereo = render(spu2, 8);
    checkLevel(stereo[14], kFlatSample, "the left channel carries the voice");
    checkEqual(stereo[15], static_cast<s16>(0), "the right channel is silent");
}

// Waveform data with an endpoint and no loop stops the voice and leaves the
// envelope reading zero.
void checkSpu2StopsAtAnEndpoint() {
    uee::Spu2 spu2;
    programVoice(spu2, 0x1000);
    spu2.receive(0, makeFlatBlock(0x01));
    spu2.writeHalf(kCoreKeyOn, 1);

    render(spu2, uee::kAdpcmBlockSamples + 2);
    checkEqual(spu2.readHalf(kCoreEndx).value_or(0), static_cast<u16>(1),
               "the voice reports that it ended");
    checkEqual(spu2.readHalf(kVoiceEnvelope).value_or(1), static_cast<u16>(0),
               "the envelope reads zero once it has");
}

// An endpoint block that also loops sends generation back to the loop point
// instead of stopping.
void checkSpu2LoopsBackToItsLoopPoint() {
    uee::Spu2 spu2;
    programVoice(spu2, 0x1000);
    spu2.receive(0, makeFlatBlock(0x07));
    spu2.writeHalf(kCoreKeyOn, 1);

    const std::vector<s16> stereo = render(spu2, uee::kAdpcmBlockSamples * 3);
    checkLevel(stereo.back(), kFlatSample, "the voice is still playing after the endpoint");
    checkEqual(spu2.readHalf(kVoiceEnvelope).value_or(0), static_cast<u16>(0x7FFF),
               "a looping voice keeps its envelope");
}

// Half the pitch takes twice as long to reach the end of the data.
void checkSpu2PitchSetsThePace() {
    uee::Spu2 slow;
    programVoice(slow, 0x0800);
    slow.receive(0, makeFlatBlock(0x01));
    slow.writeHalf(kCoreKeyOn, 1);

    render(slow, uee::kAdpcmBlockSamples + 2);
    checkEqual(slow.readHalf(kCoreEndx).value_or(1), static_cast<u16>(0),
               "at half pitch the block is only half read");
    render(slow, uee::kAdpcmBlockSamples + 2);
    checkEqual(slow.readHalf(kCoreEndx).value_or(0), static_cast<u16>(1),
               "twice the frames reach the end of it");
}

// A rate's period doubles every four steps, so an attack of rate 0 crosses the
// envelope in 3 samples and one of rate 0x10 takes 37.
void checkSpu2AttackMatchesThePublishedRate() {
    for (const auto& [rate, samples] : {std::pair<u16, usize>{0x00, 3}, {0x10, 37}}) {
        uee::Spu2 spu2;
        programVoice(spu2, 0x1000);
        spu2.writeHalf(kVoiceAdsr1, static_cast<u16>((rate << 8) | 0x00FF));
        spu2.receive(0, makeFlatBlock(0x07));
        spu2.writeHalf(kCoreKeyOn, 1);

        render(spu2, samples - 1);
        check(spu2.readHalf(kVoiceEnvelope).value_or(0) < 0x7FFF,
              std::format("attack {:02x} has not finished a sample early", rate));
        render(spu2, 1);
        checkEqual(spu2.readHalf(kVoiceEnvelope).value_or(0), static_cast<u16>(0x7FFF),
                   std::format("attack {:02x} reaches the top on time", rate));
    }
}

void checkGsCsrClearsOnWritingOne() {
    uee::Gs gs;
    gs.raiseFinish();
    gs.advanceScanline();
    gs.raiseSignal();
    checkEqual(gs.csr() & 3u, static_cast<u64>(3), "both interrupts are reported");

    gs.write(0x12001000, kCsrFinish);
    checkEqual(gs.csr() & 3u, static_cast<u64>(1),
               "writing a one clears only the bit it names");
    gs.write(0x12001000, 0);
    checkEqual(gs.csr() & 3u, static_cast<u64>(1), "writing a zero clears nothing");
}

int main() {
    checkQueueIsFifo();
    checkQueueRotatesOldestToNewest();
    checkAlarmsSortByTarget();
    checkAlarmTargetWraps();
    checkAlarmSortsWrappedAfterPending();
    checkAlarmReleaseReportsElapsed();
    checkAlarmSlotsRunOut();
    checkTimersCountHsync();
    checkTimersIgnoreDisabledCounters();
    checkTimerClockSelectSetsTheRate();
    checkTimerKeepsItsPlaceAcrossAClockChange();
    checkTimerRegisterWindow();
    checkInterruptMasksToggle();
    checkStatusIsWriteOneToClear();
    checkDmacMaskLivesInTheUpperHalf();
    checkHandlerChainOrder();
    checkSbusIsReservedForTheKernel();
    checkNegativeSyscallSelectsTheSameSlot();
    checkDeci2BypassesTheTableOnlyWhenPositive();
    checkTheTableIndexIsUnchecked();
    checkGuestHandlersAreCalledNotEntered();
    checkUnclaimedSlotsAreUndefinedSyscall();
    checkRegisterListPacksTwoRegistersPerQuadword();
    checkPackedSpendsAQuadwordPerRegister();
    checkImageCountsQuadwordsNotRegisters();
    checkGsHoldsFinishUntilTheScanlineIsOut();
    checkGsCsrClearsOnWritingOne();
    checkAdpcmDecodesAFlatBlock();
    checkAdpcmShiftScalesTheNibble();
    checkAdpcmFilterFollowsThePreviousSamples();
    checkAdpcmCarriesStateAcrossBlocks();
    checkAdpcmIgnoresAPartialBlock();
    checkPadAnswersAPoll();
    checkPadReportsConfigMode();
    checkPadSwitchesToAnalog();
    checkPadAnswersTheMappingItReplaces();
    checkSio2AnswersEveryQueuedCommand();
    checkSio2ReportsWhichQueueEntryFoundNothing();
    checkSio2KeepsTheCommandWrittenAfterAReset();
    checkSio2StopsAtAnEmptyEntry();
    checkSpu2PlaysAKeyedOnVoice();
    checkSpu2PansAVoice();
    checkSpu2StopsAtAnEndpoint();
    checkSpu2LoopsBackToItsLoopPoint();
    checkSpu2PitchSetsThePace();
    checkSpu2AttackMatchesThePublishedRate();

    if (gFailures != 0) {
        spdlog::error("{} checks failed", gFailures);
        return 1;
    }
    spdlog::info("kernel primitives: all checks passed");
    return 0;
}
