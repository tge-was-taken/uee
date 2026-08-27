#include <kernel.h>
#include <stdio.h>

static int syscall3(int number, int a0, int a1, int a2)
{
    register int n asm("$3") = number;
    register int r asm("$2");
    register int x asm("$4") = a0;
    register int y asm("$5") = a1;
    register int z asm("$6") = a2;
    asm volatile("syscall" : "=r"(r) : "r"(n), "r"(x), "r"(y), "r"(z) : "memory");
    return r;
}

// Every call here reaches a subsystem this runtime does not model. The point of
// the test is that the guest keeps running and can see the failure.
int main(void)
{
    printf("-- TEST BEGIN\n");

    printf("SetGsCrt: %d\n", syscall3(2, 2, 2, 1));
    printf("LoadExecPS2: %d\n", syscall3(6, 0, 0, 0));
    printf("ExecOSD: %d\n", syscall3(123, 0, 0, 0));
    printf("Deci2Call: %d\n", syscall3(124, 1, 0, 0));
    printf("undefined slot 86: %d\n", syscall3(86, 0, 0, 0));

    printf("still running after all of them\n");
    printf("-- TEST END\n");
    return 0;
}
