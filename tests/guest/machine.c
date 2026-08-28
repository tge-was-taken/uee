#include <kernel.h>
#include <stdio.h>

#include "syscall.h"

enum {
    kSetOsdConfigParam = 74,
    kGetOsdConfigParam = 75,
    kFlushCache = 100,
    kGetCop0 = 99,
    kSetupHeap = 61,
    kEndOfHeap = 62,
    kPsMode = 125,
    kMachineType = 126,
    kGetMemorySize = 127,
};

static u8 gHeap[0x2000] __attribute__((aligned(64)));

int main(void)
{
    ee_thread_status_t info;
    unsigned int osdBefore, osdRead, config;
    int sized, bounded, unbounded, original;
    int typeBefore, typeAfter;

    printf("-- TEST BEGIN\n");

    printf("memory size: %08x\n", syscall0(kGetMemorySize));

    // PSMode raises a bit in the same word MachineType reports.
    typeBefore = syscall0(kMachineType);
    syscall0(kPsMode);
    typeAfter = syscall0(kMachineType);
    printf("machine type: ps bit was %d, now %d\n", (typeBefore & 0x8000) != 0,
           (typeAfter & 0x8000) != 0);

    // The heap end belongs to the thread, and a negative size runs it up to the
    // thread's own stack. Everything is captured before printing because printf
    // allocates out of whatever this leaves behind.
    ReferThreadStatus(0, &info);
    original = syscall0(kEndOfHeap);
    syscall2(kSetupHeap, (int)gHeap, sizeof(gHeap));
    bounded = syscall0(kEndOfHeap);
    syscall2(kSetupHeap, (int)gHeap, -1);
    unbounded = syscall0(kEndOfHeap);
    syscall2(kSetupHeap, original, 0);
    sized = syscall0(kEndOfHeap);

    printf("heap sized: %d\n", bounded == (int)gHeap + (int)sizeof(gHeap) ? 1 : 0);
    printf("heap to stack: %d\n", unbounded == (int)info.stack ? 1 : 0);
    printf("heap restored: %d\n", sized == original ? 1 : 0);

    // Every bit of the OSD word survives the round trip.
    syscall1(kGetOsdConfigParam, (int)&osdBefore);
    config = 0x5AA53CC3;
    syscall1(kSetOsdConfigParam, (int)&config);
    syscall1(kGetOsdConfigParam, (int)&osdRead);
    syscall1(kSetOsdConfigParam, (int)&osdBefore);
    printf("osd round trip: %08x\n", osdRead);

    printf("flush: %d %d %d %d\n", syscall1(kFlushCache, 0), syscall1(kFlushCache, 1),
           syscall1(kFlushCache, 2), syscall1(kFlushCache, 3));

    printf("prid nonzero: %d\n", syscall1(kGetCop0, 15) != 0);
    printf("threads alive: %d\n", getThreadCount() >= 1);

    printf("-- TEST END\n");
    return 0;
}
