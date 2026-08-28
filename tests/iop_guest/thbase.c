#include <kerr.h>
#include <stdio.h>
#include <sysclib.h>
#include <tamtypes.h>
#include <thbase.h>

static int isHandle(int id)
{
    return id > 0 && (id & 1) != 0;
}

static volatile int gRan;
static volatile int gArgument;

static void worker(void *arg)
{
    gArgument = (int)arg;
    gRan = 1;
    ExitThread();
}

static void fill(iop_thread_t *thread)
{
    thread->attr = TH_C;
    thread->option = 0;
    thread->thread = worker;
    thread->stacksize = 2048;
    thread->priority = 40;
}

static void lifecycle(void)
{
    iop_thread_t thread;
    iop_thread_info_t info;
    int self, id;

    self = GetThreadId();
    printf("self: handle %d\n", isHandle(self));

    if (ReferThreadStatus(self, &info) == 0) {
        printf("self status: %02x, priority in user range %d\n", info.status,
               info.currentPriority >= USER_HIGHEST_PRIORITY &&
                   info.currentPriority <= USER_LOWEST_PRIORITY);
    }
    printf("refer self by 0: %d\n", ReferThreadStatus(TH_SELF, &info) == 0);

    fill(&thread);
    id = CreateThread(&thread);
    printf("create: handle %d\n", isHandle(id));
    if (ReferThreadStatus(id, &info) == 0) {
        printf("dormant: status %02x, priority %d\n", info.status, info.currentPriority);
    }

    // A thread that has never run can be deleted; one that has been started
    // cannot, until it is back to dormant.
    printf("delete dormant: %d\n", DeleteThread(id));

    id = CreateThread(&thread);
    gRan = 0;
    gArgument = 0;
    printf("start: %d\n", StartThread(id, (void *)0x1234) == id);
    printf("start twice: %d\n", StartThread(id, 0));
    printf("ran: %d, argument %04x\n", gRan, gArgument);
    printf("delete finished: %d\n", DeleteThread(id));
}

static void rejections(void)
{
    iop_thread_t thread;
    iop_thread_info_t info;

    fill(&thread);
    thread.attr = 0x40;
    printf("bad attr: %d\n", CreateThread(&thread));

    fill(&thread);
    thread.thread = 0;
    printf("no entry: %d\n", CreateThread(&thread));

    fill(&thread);
    thread.priority = 0;
    printf("priority 0: %d\n", CreateThread(&thread));
    thread.priority = 127;
    printf("priority 127: %d\n", CreateThread(&thread));

    fill(&thread);
    thread.stacksize = 0;
    printf("no stack: %d\n", CreateThread(&thread));

    printf("unknown thid: %d %d %d\n", StartThread(1, 0), DeleteThread(1),
           ReferThreadStatus(1, &info));
    printf("wakeup unknown: %d\n", WakeupThread(1));
    printf("priority unknown: %d\n", ChangeThreadPriority(1, 50));
}

static void priority(void)
{
    iop_thread_t thread;
    iop_thread_info_t info;
    int self, previous;

    self = GetThreadId();
    ReferThreadStatus(self, &info);
    previous = info.currentPriority;

    printf("change: %d\n", ChangeThreadPriority(self, 60));
    ReferThreadStatus(self, &info);
    printf("took effect: %d, initial unchanged %d\n", info.currentPriority == 60,
           info.initPriority == previous);
    ChangeThreadPriority(self, previous);

    printf("out of range: %d %d\n", ChangeThreadPriority(self, 0),
           ChangeThreadPriority(self, 127));

    fill(&thread);
    printf("dormant priority: %d\n", ChangeThreadPriority(CreateThread(&thread), 50));
}

// A wakeup that arrives before the sleep is banked, so the sleep returns at
// once instead of blocking with nothing left to wake it.
static void sleeping(void)
{
    int self = GetThreadId();
    int banked;

    printf("bank a wakeup: %d\n", WakeupThread(self));
    printf("sleep returns: %d\n", SleepThread());
    printf("cancel with none banked: %d\n", CancelWakeupThread(self));
    banked = WakeupThread(self);
    printf("bank then cancel: %d %d\n", banked, CancelWakeupThread(self));
}

static void clocks(void)
{
    iop_sys_clock_t clock, converted;
    u32 seconds, microseconds;

    printf("system time: %d\n", GetSystemTime(&clock) == 0);

    USec2SysClock(1000000, &converted);
    SysClock2USec(&converted, &seconds, &microseconds);
    printf("one second: %d %d\n", seconds, microseconds);

    USec2SysClock(1500000, &converted);
    SysClock2USec(&converted, &seconds, &microseconds);
    printf("one and a half: %d %d\n", seconds, microseconds);

    printf("delay: %d\n", DelayThread(1000));
}

int _start(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    printf("-- TEST BEGIN\n");
    lifecycle();
    rejections();
    priority();
    sleeping();
    clocks();
    printf("-- TEST END\n");
    return 0;
}
