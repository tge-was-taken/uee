#include <kernel.h>
#include <stdio.h>
#include <string.h>

#include "syscall.h"

static char gOutput[8192];

static u8 gStacks[3][4096] __attribute__((aligned(16)));
static s32 gBlocker;
static volatile s32 gSleepResult = 0x7FFF;
static volatile int gSlept;

static void sleeper(void *arg)
{
    (void)arg;
    gSleepResult = SleepThread();
    gSlept = 1;
    ExitThread();
}

static void semaWaiter(void *arg)
{
    (void)arg;
    WaitSema(gBlocker);
    ExitThread();
}

static void idle(void *arg)
{
    (void)arg;
    for (;;) {
        SleepThread();
    }
}

static s32 create(void (*entry)(void *), int index, int priority)
{
    ee_thread_t param;
    memset(&param, 0, sizeof(param));
    param.func = entry;
    param.stack = gStacks[index];
    param.stack_size = sizeof(gStacks[index]);
    param.gp_reg = &_gp;
    param.initial_priority = priority;
    return CreateThread(&param);
}

static int statusOf(s32 id)
{
    ee_thread_status_t info;
    return ReferThreadStatus(id, &info);
}

// There is no yield syscall, so dropping below the workers is what lets them
// reach the state the next assertion inspects.
static void yield(s32 self)
{
    ChangeThreadPriority(self, 60);
    ChangeThreadPriority(self, 40);
}

int main(void)
{
    ee_thread_status_t info;
    ee_sema_t sema;
    s32 self, sleep, waiter, spare, banked, suspended;

    // printf blocks on the console RPC, which lets a lower-priority thread run
    // at every call and perturbs the schedule under test. Full buffering keeps
    // the whole run to one flush at exit.
    setvbuf(stdout, gOutput, _IOFBF, sizeof(gOutput));

    printf("-- TEST BEGIN\n");

    self = GetThreadId();
    ChangeThreadPriority(self, 40);

    memset(&sema, 0, sizeof(sema));
    sema.max_count = 1;
    gBlocker = CreateSema(&sema);

    sleep = create(sleeper, 0, 50);
    waiter = create(semaWaiter, 1, 50);
    spare = create(idle, 2, 50);

    printf("dormant: status %02x\n", statusOf(sleep));
    printf("suspend dormant: %d\n", (int)SuspendThread(sleep));
    printf("wakeup dormant: %d\n", (int)WakeupThread(sleep));
    printf("resume dormant: %d\n", (int)ResumeThread(sleep));

    StartThread(spare, 0);
    printf("ready: status %02x\n", statusOf(spare));
    suspended = SuspendThread(spare);
    printf("suspend ready: %d, status %02x\n", suspended >= 0 ? 1 : 0, statusOf(spare));

    // A suspended thread only banks the wakeup; it does not become ready.
    printf("wakeup suspended: %d, status %02x\n", (int)WakeupThread(spare) >= 0 ? 1 : 0,
           statusOf(spare));
    ReferThreadStatus(spare, &info);
    printf("banked wakeups: %d\n", (int)info.wakeupCount);
    banked = CancelWakeupThread(spare);
    printf("cancel wakeup: %d, again %d\n", (int)banked, (int)CancelWakeupThread(spare));

    printf("resume suspended: %d, status %02x\n", (int)ResumeThread(spare) >= 0 ? 1 : 0,
           statusOf(spare));
    printf("resume ready: %d\n", (int)ResumeThread(spare));

    // Sleeping carries wait_type 1, which is the only kind WakeupThread clears.
    StartThread(sleep, 0);
    yield(self);
    printf("sleeping: status %02x, waitType %d\n", statusOf(sleep),
           ReferThreadStatus(sleep, &info) >= 0 ? (int)info.waitType : -1);

    printf("suspend waiting: %d, status %02x\n", (int)SuspendThread(sleep) >= 0 ? 1 : 0,
           statusOf(sleep));
    printf("resume waiting: %d, status %02x\n", (int)ResumeThread(sleep) >= 0 ? 1 : 0,
           statusOf(sleep));

    printf("wakeup sleeper: %d\n", (int)WakeupThread(sleep) >= 0 ? 1 : 0);
    yield(self);
    printf("sleeper returned own id: %d, ran %d\n", gSleepResult == sleep ? 1 : 0, gSlept);

    // A semaphore wait carries wait_type 2, so WakeupThread only counts it.
    StartThread(waiter, 0);
    yield(self);
    printf("sema wait: status %02x, waitType %d, waitId %d\n", statusOf(waiter),
           ReferThreadStatus(waiter, &info) >= 0 ? (int)info.waitType : -1,
           (int)info.waitId == gBlocker ? 1 : -1);
    printf("wakeup sema waiter: %d, status %02x\n", (int)WakeupThread(waiter) >= 0 ? 1 : 0,
           statusOf(waiter));

    printf("wakeup self: %d\n", (int)WakeupThread(self));
    printf("resume self: %d\n", (int)ResumeThread(self));
    printf("suspend id 0: %d\n", (int)SuspendThread(0));
    printf("suspend id 256: %d\n", (int)SuspendThread(256));

    printf("refer null: %02x\n", (int)ReferThreadStatus(self, 0));
    printf("refer thid 0: %02x\n", (int)ReferThreadStatus(0, &info));

    SignalSema(gBlocker);
    printf("released: status %02x\n", statusOf(waiter));

    printf("-- TEST END\n");
    fflush(stdout);
    return 0;
}
