#include <kernel.h>
#include <stdio.h>
#include <string.h>

#include "syscall.h"

// A thread that blocks inside a nested call is picked up again part way through
// every function on the way down, and what those functions do on the way back
// out is what the translated path has to get right.

static u8 gStacks[2][8192] __attribute__((aligned(16)));

static s32 gGate;
static volatile int gSteps;

// Reached by a tail call, so its answer is returned as the answer of whoever
// tail called it.
static int __attribute__((noinline)) finish(int value)
{
    gSteps += 1;
    return value + 1;
}

// Blocks, so it is resumed part way through, and then leaves by a tail call.
static int __attribute__((noinline)) blockThenTail(int value)
{
    WaitSema(gGate);
    gSteps += 1;
    return finish(value);
}

// Waiting on a call that blocks, so this one is resumed part way through too.
static int __attribute__((noinline)) callThroughBlocking(int value)
{
    int inner = blockThenTail(value);
    gSteps += 1;
    return inner + 1;
}

// Two frames deep, so the resume unwinds through more than one of them.
static int __attribute__((noinline)) outer(int value)
{
    return callThroughBlocking(value) + 1;
}

static void opener(void *arg)
{
    (void)arg;
    SignalSema(gGate);
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

int main(void)
{
    ee_sema_t sema;
    s32 self, waker;
    int answer;

    printf("-- TEST BEGIN\n");

    self = GetThreadId();
    ChangeThreadPriority(self, 20);

    memset(&sema, 0, sizeof(sema));
    sema.max_count = 1;
    gGate = CreateSema(&sema);

    // Lower priority, so it only runs once this thread blocks on the gate.
    waker = create(opener, 0, 50);
    StartThread(waker, 0);

    gSteps = 0;
    answer = outer(10);
    printf("answer: %d\n", answer);
    printf("steps: %d\n", gSteps);

    // Again, blocking again, to show the resume left nothing behind.
    waker = create(opener, 1, 50);
    StartThread(waker, 0);
    gSteps = 0;
    answer = outer(20);
    printf("answer again: %d\n", answer);
    printf("steps again: %d\n", gSteps);

    DeleteSema(gGate);
    printf("-- TEST END\n");
    return 0;
}
