#pragma once

// $v1 carries the syscall number and a negative number selects the
// interrupt-context variant of the same slot.
static inline int syscall0(int n)
{
    register int number asm("$3") = n;
    register int result asm("$2");
    asm volatile("syscall" : "=r"(result) : "r"(number) : "memory");
    return result;
}

static inline int syscall1(int n, int a0)
{
    register int number asm("$3") = n;
    register int result asm("$2");
    register int x asm("$4") = a0;
    asm volatile("syscall" : "=r"(result) : "r"(number), "r"(x) : "memory");
    return result;
}

static inline int syscall2(int n, int a0, int a1)
{
    register int number asm("$3") = n;
    register int result asm("$2");
    register int x asm("$4") = a0;
    register int y asm("$5") = a1;
    asm volatile("syscall" : "=r"(result) : "r"(number), "r"(x), "r"(y) : "memory");
    return result;
}

static inline int syscall3(int n, int a0, int a1, int a2)
{
    register int number asm("$3") = n;
    register int result asm("$2");
    register int x asm("$4") = a0;
    register int y asm("$5") = a1;
    register int z asm("$6") = a2;
    asm volatile("syscall" : "=r"(result) : "r"(number), "r"(x), "r"(y), "r"(z) : "memory");
    return result;
}

static inline int getThreadCount(void) { return syscall0(80); }
static inline int getSemaCount(void) { return syscall0(81); }
static inline int getAlarmCount(void) { return syscall0(82); }
static inline int getIntrHandlerCount(void) { return syscall0(83); }
