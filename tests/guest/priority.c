#include <kernel.h>
#include <stdio.h>
#include <string.h>

#include "syscall.h"

static volatile char gLog[16];
static volatile int gLogLength;

static void note(char c)
{
    if (gLogLength < (int)sizeof(gLog) - 1) {
        gLog[gLogLength++] = c;
        gLog[gLogLength] = 0;
    }
}

static u8 gStacks[4][4096] __attribute__((aligned(16)));

static void marker(void *arg)
{
    note((char)(long)arg);
    ExitThread();
}

static s32 create(void (*entry)(void *), int index, int initial, int current)
{
    ee_thread_t param;
    memset(&param, 0, sizeof(param));
    param.func = entry;
    param.stack = gStacks[index];
    param.stack_size = sizeof(gStacks[index]);
    param.gp_reg = &_gp;
    param.initial_priority = initial;
    param.current_priority = current;
    return CreateThread(&param);
}

int main(void)
{
    ee_thread_status_t info;
    s32 self, worker, low, high, previous;

    printf("-- TEST BEGIN\n");

    self = GetThreadId();
    previous = ChangeThreadPriority(self, 40);
    printf("main: prio was valid %d, now %d\n", previous >= 0 ? 1 : 0,
           ReferThreadStatus(self, &info) >= 0 ? info.current_priority : -99);

    // ChangeThreadPriority answers with the priority it replaced. P3's adxm_lock
    // keeps that value and restores it on unlock.
    printf("swap: returns previous %d\n", (int)ChangeThreadPriority(self, 41) == 40 ? 1 : 0);
    printf("thid 0 is current: %d\n", (int)ChangeThreadPriority(0, 40) == 41 ? 1 : 0);

    printf("prio 0x80: %d\n", (int)ChangeThreadPriority(self, 0x80));
    printf("prio 0x7f: %d\n", (int)ChangeThreadPriority(self, 0x7F) >= 0 ? 1 : 0);
    ChangeThreadPriority(self, 40);

    // P3's InitThread creates its deferred-operation worker at priority 0, and
    // CreateThread range-checks nothing, so this has to be accepted.
    high = create(marker, 0, 0, 0);
    printf("create prio 0: %d\n", high >= 0 ? 1 : 0);

    // current_priority in the parameter block is ignored; both fields come from
    // initial_priority.
    worker = create(marker, 1, 50, 7);
    if (ReferThreadStatus(worker, &info) >= 0) {
        printf("create: initial %d, current %d\n", info.initial_priority, info.current_priority);
    }
    printf("dormant change: %d\n", (int)ChangeThreadPriority(worker, 51));

    // A lower-priority thread becomes ready without displacing us; raising it
    // above our level hands the processor over inside the syscall.
    gLogLength = 0;
    StartThread(worker, (void *)'W');
    note('m');
    ChangeThreadPriority(worker, 30);
    printf("preempt: %s\n", (char *)gLog);

    printf("rotate 0x80: %d\n", (int)RotateThreadReadyQueue(0x80));
    printf("rotate 60: %d\n", (int)RotateThreadReadyQueue(60) == 60 ? 1 : 0);

    // Two ready threads at one level run oldest first, and rotating the level
    // puts the younger one in front.
    gLogLength = 0;
    low = create(marker, 2, 60, 60);
    StartThread(low, (void *)'A');
    StartThread(create(marker, 3, 60, 60), (void *)'B');
    RotateThreadReadyQueue(60);
    ChangeThreadPriority(self, 70);
    printf("rotated: %s\n", (char *)gLog);

    gLogLength = 0;
    StartThread(high, (void *)'H');
    printf("prio 0 ran: %s\n", (char *)gLog);

    printf("-- TEST END\n");
    return 0;
}
