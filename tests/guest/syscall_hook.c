#include <kernel.h>
#include <stdio.h>

#include "syscall.h"

enum {
    kGetThreadId = 47,
    kSpare = 84,
};

// The retail kernel keeps its syscall table here and nothing publishes the
// address, so a guest that wants to save a slot has to read it directly. P3FES
// searches for the same table before installing its own handlers.
static volatile unsigned int *const kSyscallTable = (volatile unsigned int *)0x80014F40;

static int gCalls;

// Reached through the syscall table, so it returns with jr $ra like the ROM
// handlers the dispatcher normally lands on.
static int spareHandler(int a0, int a1)
{
    ++gCalls;
    return a0 + a1;
}

static int fakeThreadId(void)
{
    return 0x1234;
}

static int isRomHandler(unsigned int handler)
{
    return (handler & 0x1FFFFFFFu) < 0x16E28u && handler >= 0x80000000u;
}

int main(void)
{
    unsigned int spareStub, threadIdStub;
    s32 self;

    printf("-- TEST BEGIN\n");

    self = GetThreadId();

    // P3FES installs its own routines in slots the ROM leaves as the undefined
    // stub, then calls them as ordinary syscalls. ps2sdk's libkernel does the
    // same thing with slots 90 and 91, which is why this uses neither.
    spareStub = kSyscallTable[kSpare];
    printf("spare holds a rom stub: %d\n", isRomHandler(spareStub));

    SetSyscall(kSpare, (void *)spareHandler);
    printf("installed: %d\n", kSyscallTable[kSpare] == (unsigned int)spareHandler);
    printf("guest handler: %d, calls %d\n", syscall2(kSpare, 20, 22), gCalls);
    printf("again: %d, calls %d\n", syscall2(kSpare, 1, 2), gCalls);

    SetSyscall(kSpare, (void *)spareStub);
    printf("stub restored: %d\n", kSyscallTable[kSpare] == spareStub);

    // Replacing a live slot has to take effect, and putting the ROM address
    // back has to bring the built-in implementation with it.
    threadIdStub = kSyscallTable[kGetThreadId];
    printf("live slot is rom: %d\n", isRomHandler(threadIdStub));
    SetSyscall(kGetThreadId, (void *)fakeThreadId);
    printf("hooked: %d\n", syscall0(kGetThreadId));
    SetSyscall(kGetThreadId, (void *)threadIdStub);
    printf("unhooked: %d\n", syscall0(kGetThreadId) == self);

    printf("-- TEST END\n");
    return 0;
}
