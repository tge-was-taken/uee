#include <kernel.h>
#include <stdio.h>
#include <string.h>

#include "syscall.h"

static u8 gStacks[5][4096] __attribute__((aligned(16)));

static s32 gOrderSema;
static s32 gSignalSema;
static s32 gDeleteSema;
static volatile s32 gResult[5];

static void queueUp(void *arg)
{
    int slot = (int)(long)arg;
    gResult[slot] = WaitSema(gOrderSema);
    ExitThread();
}

static void waitForSignal(void *arg)
{
    gResult[(int)(long)arg] = WaitSema(gSignalSema);
    ExitThread();
}

static void waitForDelete(void *arg)
{
    gResult[(int)(long)arg] = WaitSema(gDeleteSema);
    ExitThread();
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

static void yield(s32 self)
{
    ChangeThreadPriority(self, 60);
    ChangeThreadPriority(self, 20);
}

static int statusOf(s32 id)
{
    ee_thread_status_t info;
    return ReferThreadStatus(id, &info);
}

static s32 blockOne(s32 self, void (*entry)(void *), int index, int priority, int slot)
{
    s32 id = create(entry, index, priority);
    StartThread(id, (void *)(long)slot);
    yield(self);
    return id;
}

int main(void)
{
    ee_sema_t sema;
    s32 self, a, b, c, signalled, deleted, id, first, second;
    int before;

    printf("-- TEST BEGIN\n");

    self = GetThreadId();
    ChangeThreadPriority(self, 20);
    before = getSemaCount();

    memset(&sema, 0, sizeof(sema));
    sema.init_count = -1;
    sema.max_count = 1;
    printf("negative init: %d\n", (int)CreateSema(&sema));

    // count comes from init_count; the count field of the parameter block is
    // never read.
    memset(&sema, 0, sizeof(sema));
    sema.count = 99;
    sema.init_count = 2;
    sema.max_count = 4;
    id = CreateSema(&sema);
    if (ReferSemaStatus(id, &sema) >= 0) {
        printf("created: count %d, max %d, waiters %d\n", sema.count, sema.max_count,
               sema.wait_threads);
    }
    printf("refer returns id: %d\n", (int)ReferSemaStatus(id, &sema) == id ? 1 : 0);
    printf("sema count grew: %d\n", getSemaCount() - before);

    first = PollSema(id);
    second = PollSema(id);
    printf("poll: %d %d %d\n", first == id ? 1 : 0, second == id ? 1 : 0, (int)PollSema(id));
    printf("poll bad id: %d\n", (int)PollSema(300));
    DeleteSema(id);
    printf("poll deleted: %d\n", (int)PollSema(id));
    printf("refer deleted: %d\n", (int)ReferSemaStatus(id, &sema));
    printf("refer out of range: %d\n", (int)ReferSemaStatus(300, &sema));

    // The wait list is FIFO, so the oldest waiter is released first even when a
    // younger one has a higher priority.
    memset(&sema, 0, sizeof(sema));
    sema.max_count = 4;
    gOrderSema = CreateSema(&sema);
    a = blockOne(self, queueUp, 0, 50, 0);
    b = blockOne(self, queueUp, 1, 30, 1);
    c = blockOne(self, queueUp, 2, 40, 2);
    ReferSemaStatus(gOrderSema, &sema);
    printf("queued: waiters %d, count %d\n", sema.wait_threads, sema.count);

    SignalSema(gOrderSema);
    printf("after 1: %x %x %x\n", statusOf(a), statusOf(b), statusOf(c));
    ReferSemaStatus(gOrderSema, &sema);
    printf("handover keeps count: %d, waiters %d\n", sema.count, sema.wait_threads);
    SignalSema(gOrderSema);
    printf("after 2: %x %x %x\n", statusOf(a), statusOf(b), statusOf(c));
    SignalSema(gOrderSema);
    printf("after 3: %x %x %x\n", statusOf(a), statusOf(b), statusOf(c));
    SignalSema(gOrderSema);
    ReferSemaStatus(gOrderSema, &sema);
    printf("spare signal counts: %d\n", sema.count);
    yield(self);
    printf("released waiters saw: %d %d %d\n", (int)gResult[0] == gOrderSema ? 1 : 0,
           (int)gResult[1] == gOrderSema ? 1 : 0, (int)gResult[2] == gOrderSema ? 1 : 0);
    DeleteSema(gOrderSema);

    // A signal clears wait_type and the waiter sees the semaphore id; a delete
    // leaves wait_type set and the same call comes back -1.
    memset(&sema, 0, sizeof(sema));
    sema.max_count = 1;
    gSignalSema = CreateSema(&sema);
    gDeleteSema = CreateSema(&sema);
    signalled = blockOne(self, waitForSignal, 3, 50, 3);
    deleted = blockOne(self, waitForDelete, 4, 50, 4);
    SignalSema(gSignalSema);
    DeleteSema(gDeleteSema);
    yield(self);
    printf("signalled saw id: %d\n", (int)gResult[3] == gSignalSema ? 1 : 0);
    printf("deleted saw: %d\n", (int)gResult[4]);
    printf("both finished: %d\n", statusOf(signalled) == 0x10 && statusOf(deleted) == 0x10);

    printf("wait bad id: %d\n", (int)WaitSema(300));
    printf("signal bad id: %d\n", (int)SignalSema(300));
    printf("delete twice: %d\n", (int)DeleteSema(gDeleteSema));

    DeleteSema(gSignalSema);
    printf("balanced: %d\n", getSemaCount() - before);

    printf("-- TEST END\n");
    return 0;
}
