#include <stdio.h>
#include <tamtypes.h>

// Shapes a translator has to get right, from the trivial upward. Each one has a
// short best-case x64 form, so the cost of translating it can be read against
// something rather than against itself.

static int gInts[256];
static float gFloats[256];

struct Point {
    float x, y, z, w;
};

struct Node {
    struct Node *next;
    int value;
};

static struct Node gNodes[32];

static struct Point gPoints[64];

// 1. Pure arithmetic on values already in registers.
int __attribute__((noinline)) shapeAdd(int a, int b)
{
    return a + b;
}

// 2. Arithmetic with a constant, which the guest builds with lui/ori.
int __attribute__((noinline)) shapeConstant(int a)
{
    return a * 1103515245 + 12345;
}

// 3. One load from a global at a fixed address.
int __attribute__((noinline)) shapeLoadGlobal(void)
{
    return gInts[7];
}

// 4. One load through a pointer the caller supplies.
int __attribute__((noinline)) shapeLoadPointer(const int *p)
{
    return p[3];
}

// 5. A load and a store: the shape of every field update.
void __attribute__((noinline)) shapeCopyField(struct Point *dst, const struct Point *src)
{
    dst->y = src->x;
}

// 6. A counted loop with one load and one add.
int __attribute__((noinline)) shapeSumLoop(const int *p, int count)
{
    int total = 0;
    int i;
    for (i = 0; i < count; ++i) {
        total += p[i];
    }
    return total;
}

// 7. A loop over a stride, which is what array-of-struct traversal looks like.
float __attribute__((noinline)) shapeSumStride(const struct Point *p, int count)
{
    float total = 0.0f;
    int i;
    for (i = 0; i < count; ++i) {
        total += p[i].x;
    }
    return total;
}

// 8. A branch with two arms.
int __attribute__((noinline)) shapeSelect(int a, int b)
{
    return (a > b) ? a - b : b - a;
}

// 9. A call, which is where a translator has to give up cached state.
int __attribute__((noinline)) shapeCall(int a)
{
    return shapeAdd(a, 1) + shapeAdd(a, 2);
}

// 10. Floating point, where the guest format is not the host's.
float __attribute__((noinline)) shapeFloat(float a, float b)
{
    return a * b + a;
}

// 11. Nothing at all: the floor for what a call costs.
int __attribute__((noinline)) shapeReturnZero(void)
{
    return 0;
}

// 12. A constant that needs both halves of a lui/ori pair.
int __attribute__((noinline)) shapeBigConstant(void)
{
    return 0x12345678;
}

// 13. The argument straight back, which should be nothing at all.
int __attribute__((noinline)) shapeIdentity(int a)
{
    return a;
}

// 14. The narrow loads, which sign or zero extend on the way in.
int __attribute__((noinline)) shapeLoadByte(const signed char *p)
{
    return p[1];
}

int __attribute__((noinline)) shapeLoadHalf(const unsigned short *p)
{
    return p[1];
}

// 15. A doubleword, which the guest does in one instruction and the host in one.
long long __attribute__((noinline)) shapeLoadDouble(const long long *p)
{
    return p[1];
}

// 16. One store, nothing else.
void __attribute__((noinline)) shapeStore(int *p, int value)
{
    p[2] = value;
}

// 17. An index the caller supplies, so the address is arithmetic not a constant.
int __attribute__((noinline)) shapeIndex(const int *p, int i)
{
    return p[i];
}

// 18. A chain of loads, which is what following a data structure looks like.
int __attribute__((noinline)) shapeChase(struct Node *n)
{
    return n->next->next->value;
}

// 19. A read, modify and write of the same place.
void __attribute__((noinline)) shapeIncrement(int *p)
{
    *p += 1;
}

// 20. The copy loop, byte at a time.
void __attribute__((noinline)) shapeCopyBytes(char *dst, const char *src, int count)
{
    int i;
    for (i = 0; i < count; ++i) {
        dst[i] = src[i];
    }
}

// 21. A loop that stops on a value rather than a count.
int __attribute__((noinline)) shapeLength(const char *s)
{
    const char *start = s;
    while (*s != 0) {
        ++s;
    }
    return (int)(s - start);
}

// 22. Two nested loops over a matrix, the shape of most real work.
float __attribute__((noinline)) shapeMatrix(const float *a, const float *b, int n)
{
    float total = 0.0f;
    int i, j;
    for (i = 0; i < n; ++i) {
        for (j = 0; j < n; ++j) {
            total += a[i * 4 + j] * b[j * 4 + i];
        }
    }
    return total;
}

// 23. An early return, so the function has more than one way out.
int __attribute__((noinline)) shapeEarlyOut(const int *p, int count)
{
    int i;
    for (i = 0; i < count; ++i) {
        if (p[i] < 0) {
            return i;
        }
    }
    return -1;
}

// 24. A switch, which the compiler turns into a jump table.
int __attribute__((noinline)) shapeSwitch(int selector, int a)
{
    switch (selector) {
        case 0: return a + 1;
        case 1: return a - 1;
        case 2: return a * 2;
        case 3: return a / 3;
        case 4: return a ^ 0x5A;
        case 5: return a << 3;
        case 6: return a >> 2;
        default: return a;
    }
}

// 25. A hardware register, reached the way a game reaches one: a constant
// through a macro, which is what makes it something a translator can resolve.
#define GS_CSR ((volatile unsigned long *)0x12001000)
#define D2_CHCR ((volatile unsigned int *)0x1000A000)

unsigned int __attribute__((noinline)) shapeReadRegister(void)
{
    return (unsigned int)*GS_CSR;
}

void __attribute__((noinline)) shapeWriteRegister(unsigned int value)
{
    *D2_CHCR = value;
}

// 26. The poll a game does while it waits for hardware, which is the shape the
// register path actually runs in.
unsigned int __attribute__((noinline)) shapePollRegister(int limit)
{
    unsigned int seen = 0;
    int i;
    for (i = 0; i < limit; ++i) {
        seen += (unsigned int)*GS_CSR & 8u;
    }
    return seen;
}

int main(void)
{
    int i;
    int total = 0;

    printf("-- TEST BEGIN\n");
    for (i = 0; i < 256; ++i) {
        gInts[i] = i * 7 + 1;
        gFloats[i] = (float)i * 0.318309f;
    }
    for (i = 0; i < 64; ++i) {
        gPoints[i].x = (float)i * 1.414213f;
        gPoints[i].y = 0.0f;
    }

    total += shapeAdd(3, 4);
    total += shapeConstant(9);
    total += shapeLoadGlobal();
    total += shapeLoadPointer(gInts);
    shapeCopyField(&gPoints[1], &gPoints[2]);
    total += shapeSumLoop(gInts, 256);
    total += (int)shapeSumStride(gPoints, 64);
    total += shapeSelect(11, 4);
    total += shapeCall(5);
    total += (int)shapeFloat(1.5f, 2.5f);

    for (i = 0; i < 32; ++i) {
        gNodes[i].value = i * 3;
        gNodes[i].next = &gNodes[(i + 1) & 31];
    }
    total += shapeReturnZero();
    total += shapeBigConstant();
    total += shapeIdentity(21);
    total += shapeLoadByte((const signed char *)gInts);
    total += shapeLoadHalf((const unsigned short *)gInts);
    total += (int)shapeLoadDouble((const long long *)gInts);
    shapeStore(gInts, 99);
    total += shapeIndex(gInts, 5);
    total += shapeChase(&gNodes[0]);
    shapeIncrement(gInts);
    shapeCopyBytes((char *)gFloats, (const char *)gInts, 64);
    total += shapeLength((const char *)gInts + 1);
    total += (int)shapeMatrix(gFloats, gFloats + 16, 4);
    total += shapeEarlyOut(gInts, 16);
    total += shapeSwitch(4, 77);
    total += shapeReadRegister();
    shapeWriteRegister(0);
    total += shapePollRegister(4);

    printf("shapes %08x\n", (unsigned int)total);
    printf("-- TEST END\n");
    return 0;
}
