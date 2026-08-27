#include "alarm.h"
#include "interrupts.h"
#include "queue.h"
#include "timers.h"

#include <spdlog/spdlog.h>

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

void checkTimersCountHsync() {
    uee::Timers timers;
    timers.at(3).mode = 0x80;
    timers.advanceTo(uee::kInstructionsPerHsync * 5);
    checkEqual<u32>(timers.hsyncCount(), 5, "five ticks after five H-SYNC periods");

    timers.advanceTo(uee::kInstructionsPerHsync * 5 + uee::kInstructionsPerHsync - 1);
    checkEqual<u32>(timers.hsyncCount(), 5, "a partial period does not tick");
}

void checkTimersIgnoreDisabledCounters() {
    uee::Timers timers;
    timers.at(0).mode = 0;
    timers.at(1).mode = 0x80;
    timers.advanceTo(uee::kInstructionsPerHsync * 3);
    checkEqual<u32>(timers.at(0).count, 0, "a stopped timer does not count");
    checkEqual<u32>(timers.at(1).count, 3, "a running timer does");
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

}  // namespace

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
    checkTimerRegisterWindow();
    checkInterruptMasksToggle();
    checkStatusIsWriteOneToClear();
    checkDmacMaskLivesInTheUpperHalf();
    checkHandlerChainOrder();
    checkSbusIsReservedForTheKernel();

    if (gFailures != 0) {
        spdlog::error("{} checks failed", gFailures);
        return 1;
    }
    spdlog::info("kernel primitives: all checks passed");
    return 0;
}
