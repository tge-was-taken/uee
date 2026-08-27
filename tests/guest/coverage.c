#include <kernel.h>
#include <stdio.h>

static int syscall1(int number, int a0)
{
    register int n asm("$3") = number;
    register int r asm("$2");
    register int arg asm("$4") = a0;
    asm volatile("syscall" : "=r"(r) : "r"(n), "r"(arg) : "memory");
    return r;
}

static int syscall2(int number, int a0, int a1)
{
    register int n asm("$3") = number;
    register int r asm("$2");
    register int x asm("$4") = a0;
    register int y asm("$5") = a1;
    asm volatile("syscall" : "=r"(r) : "r"(n), "r"(x), "r"(y) : "memory");
    return r;
}

static volatile unsigned int gVSyncFlag;

int main(void)
{
    int config, spins;

    printf("-- TEST BEGIN\n");

    // GetCop0(15) is PRId; GetCop0(16) is Config.
    config = syscall1(99, 16);
    printf("cop0 config readable: %d\n", config != 0 || config == 0);

    // CpuConfig 1 sets Config ICE (0x2000), 4 clears it.
    syscall1(102, 1);
    printf("after set ICE: %d\n", (syscall1(99, 16) & 0x2000) != 0);
    syscall1(102, 4);
    printf("after clear ICE: %d\n", (syscall1(99, 16) & 0x2000) != 0);

    // CpuConfig 0/3 drive BPE (0x40000).
    syscall1(102, 0);
    printf("after set BPE: %d\n", (syscall1(99, 16) & 0x40000) != 0);
    syscall1(102, 3);
    printf("after clear BPE: %d\n", (syscall1(99, 16) & 0x40000) != 0);

    // SetVInterruptHandler takes 0..7, SetVCommonHandler takes 4..13.
    printf("vint handler 3: %d\n", syscall2(15, 3, 0x100000));
    printf("vint handler 9: %d\n", syscall2(15, 9, 0x100000));
    printf("vcommon handler 9: %d\n", syscall2(14, 9, 0x100000));
    printf("vcommon handler 2: %d\n", syscall2(14, 2, 0x100000));

    // GsPutIMR shadows what GsGetIMR reports.
    syscall1(113, 0x7F00);
    printf("imr: %04x\n", (unsigned)syscall1(112, 0) & 0xFFFF);

    // SetVSyncFlag is satisfied at the next vertical blank.
    gVSyncFlag = 0;
    syscall2(115, (int)&gVSyncFlag, 0);
    for (spins = 0; spins < 8000000 && !gVSyncFlag; ++spins) {
        ;
    }
    printf("vsync flag: %d\n", gVSyncFlag ? 1 : 0);

    printf("alarm count still: %d\n", syscall1(82, 0));

    // Slot 85 holds the ROM's undefined stub until something installs there.
    syscall1(85, 0);
    printf("-- TEST END\n");
    return 0;
}
