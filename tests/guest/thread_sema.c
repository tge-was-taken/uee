#include <kernel.h>
#include <stdio.h>
#include <string.h>

static char gLog[512];
static int gLogLength;

static void note(char c)
{
    if (gLogLength < (int)sizeof(gLog) - 1) {
        gLog[gLogLength++] = c;
    }
}

static s32 gLock;
static u8 gStacks[3][8192] __attribute__((aligned(16)));

static void waiter(void *arg)
{
    note((char)(int)(long)arg);
    WaitSema(gLock);
    note((char)((int)(long)arg + 1));
    ExitThread();
}

static s32 spawn(void (*entry)(void *), void *stack, int priority, void *arg)
{
    ee_thread_t thread;
    memset(&thread, 0, sizeof(thread));
    thread.func = entry;
    thread.stack = stack;
    thread.stack_size = 8192;
    thread.gp_reg = &_gp;
    thread.initial_priority = priority;
    return StartThread(CreateThread(&thread), arg);
}

int main(void)
{
    ee_sema_t sema;
    ee_thread_status_t info;
    s32 self, id;
    int i;

    printf("-- TEST BEGIN\n");

    self = GetThreadId();
    printf("self: id valid %d\n", self >= 0 ? 1 : 0);

    if (ReferThreadStatus(self, &info) >= 0) {
        printf("self status: status %02x\n", (unsigned)info.status);
    }

    memset(&sema, 0, sizeof(sema));
    sema.init_count = 0;
    sema.max_count = 1;
    gLock = CreateSema(&sema);
    printf("sema: created %d\n", gLock >= 0 ? 1 : 0);

    printf("poll empty: %d\n", (int)PollSema(gLock));

    // Three waiters at the same priority queue in the order they blocked, and a
    // signal has to release the oldest first.
    for (i = 0; i < 3; ++i) {
        spawn(waiter, gStacks[i] + 8192, 0, (void *)(long)('a' + i * 2));
    }
    printf("queued: %s\n", gLog);

    if (ReferSemaStatus(gLock, &sema) >= 0) {
        printf("sema waiters: %d\n", (int)sema.wait_threads);
    }

    for (i = 0; i < 3; ++i) {
        SignalSema(gLock);
    }
    printf("released: %s\n", gLog);

    // The stub never stores the worker's result on the non-blocking path, so an
    // invalid id comes straight back out.
    id = WaitSema(999);
    printf("wait bad id: %d\n", (int)id);

    printf("delete sema: %d\n", DeleteSema(gLock) >= 0 ? 1 : 0);
    printf("refer deleted: %d\n", (int)ReferSemaStatus(gLock, &sema));

    printf("-- TEST END\n");
    return 0;
}
