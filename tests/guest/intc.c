#include <kernel.h>
#include <stdio.h>
#include <string.h>

#include "syscall.h"

enum {
    kAddIntcHandler = 16,
    kRemoveIntcHandler = 17,
    kEnableIntc = 20,
    kDisableIntc = 21,
    kEnableIntcHandler = 92,
    kDisableIntcHandler = 93,
};

static volatile int gTick;
static volatile int gStamp[3];

static int stampA(int cause) { (void)cause; gStamp[0] = ++gTick; return 0; }
static int stampB(int cause) { (void)cause; gStamp[1] = ++gTick; return 0; }
static int stampC(int cause) { (void)cause; gStamp[2] = ++gTick; return 0; }

// One dispatch of a chain stamps its handlers with consecutive ticks, so a run
// of consecutive stamps is one whole pass and gives the order they ran in.
static const char *passOrder(const int *slots, int count)
{
    static char out[8];
    int seen[3];
    int spins, i, j, lo, hi;

    for (spins = 0; spins < 20000000; ++spins) {
        for (i = 0; i < count; ++i) {
            seen[i] = gStamp[slots[i]];
        }
        lo = hi = seen[0];
        for (i = 1; i < count; ++i) {
            if (seen[i] < lo) { lo = seen[i]; }
            if (seen[i] > hi) { hi = seen[i]; }
        }
        if (lo <= 0 || hi - lo != count - 1) {
            continue;
        }
        for (i = 0; i < count; ++i) {
            for (j = 0; j < count; ++j) {
                if (seen[j] == lo + i) {
                    out[i] = (char)('a' + slots[j]);
                }
            }
        }
        out[count] = 0;
        return out;
    }
    return "timed out";
}

int main(void)
{
    static const int all[3] = {0, 1, 2};
    static const int withoutA[2] = {1, 2};
    static const int tail[2] = {0, 1};
    int base, a, b, c, first, second;

    printf("-- TEST BEGIN\n");

    base = getIntrHandlerCount();
    first = syscall1(kEnableIntc, 2);
    second = syscall1(kEnableIntc, 2);
    printf("enable vblank: %d, again %d\n", first, second);

    printf("cause 15: %d\n", syscall3(kAddIntcHandler, 15, (int)stampA, 0));
    printf("cause 1 reserved: %d\n", syscall3(kAddIntcHandler, 1, (int)stampA, 0));

    // next 0 appends; next -1 puts the new handler in front of the whole chain.
    a = syscall3(kAddIntcHandler, 2, (int)stampA, 0);
    b = syscall3(kAddIntcHandler, 2, (int)stampB, 0);
    c = syscall3(kAddIntcHandler, 2, (int)stampC, -1);
    printf("ids valid and distinct: %d\n",
           a > 0 && b > 0 && c > 0 && a != b && b != c && a != c ? 1 : 0);
    printf("pool grew by: %d\n", getIntrHandlerCount() - base);
    printf("chain order: %s\n", passOrder(all, 3));

    printf("disable middle: %d\n", syscall1(kDisableIntcHandler, a) == a ? 1 : 0);
    printf("without it: %s\n", passOrder(withoutA, 2));
    printf("enable middle: %d\n", syscall1(kEnableIntcHandler, a) == a ? 1 : 0);
    printf("restored: %s\n", passOrder(all, 3));

    printf("remove head: %d\n", syscall2(kRemoveIntcHandler, 2, c));
    printf("after removal: %s\n", passOrder(tail, 2));
    printf("remove twice: %d\n", syscall2(kRemoveIntcHandler, 2, c));
    printf("remove id 0: %d\n", syscall2(kRemoveIntcHandler, 2, 0));
    printf("remove id 200: %d\n", syscall2(kRemoveIntcHandler, 2, 200));
    printf("remove bad cause: %d\n", syscall2(kRemoveIntcHandler, 20, a));

    first = syscall2(kRemoveIntcHandler, 2, b);
    second = syscall2(kRemoveIntcHandler, 2, a);
    printf("drain: %d %d\n", first, second);
    printf("pool balanced: %d\n", getIntrHandlerCount() - base);

    first = syscall1(kDisableIntc, 2);
    second = syscall1(kDisableIntc, 2);
    printf("disable vblank: %d, again %d\n", first, second);

    printf("-- TEST END\n");
    return 0;
}
