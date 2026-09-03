#include <stdio.h>
#include <string.h>
#include <tamtypes.h>

#define ARRAY 4096
#define ROUNDS 1200

static unsigned int gInts[ARRAY];
static float gFloats[ARRAY];
static unsigned char gSource[ARRAY];
static unsigned char gDestination[ARRAY];

struct Node {
    struct Node *next;
    unsigned int value;
};

static struct Node gNodes[ARRAY];

static u128 gVectors[ARRAY] __attribute__((aligned(16)));
static u128 gVectorsOut[ARRAY] __attribute__((aligned(16)));

// The low word alone hides the answer: float lanes carry zeros there, and
// enough of them add up to nothing.
static unsigned int checksum(u128 value)
{
    unsigned int words[4];
    memcpy(words, &value, sizeof(words));
    return words[0] ^ words[1] ^ words[2] ^ words[3];
}

static unsigned int floatBits(float value)
{
    unsigned int bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// The vector unit reads these as floats, so each lane is seeded as one.
static u128 lanes(float x, float y, float z, float w)
{
    return ((u128)floatBits(w) << 96) | ((u128)floatBits(z) << 64) |
           ((u128)floatBits(y) << 32) | floatBits(x);
}

// The multimedia unit, the coprocessor and the floating point unit are the
// parts a game leans on that plain C never reaches, so they are written the way
// the autotests write them.
static unsigned int mmiWork(void)
{
    unsigned int total = 0;
    int round, i;
    for (round = 0; round < ROUNDS; ++round) {
        for (i = 0; i + 1 < ARRAY; i += 2) {
            register u128 a = gVectors[i];
            register u128 b = gVectors[i + 1];
            register u128 sum, difference, high, low, packed;
            asm volatile("paddw %0, %1, %2" : "=r"(sum) : "r"(a), "r"(b));
            asm volatile("psubh %0, %1, %2" : "=r"(difference) : "r"(a), "r"(b));
            asm volatile("pmaxh %0, %1, %2" : "=r"(high) : "r"(sum), "r"(difference));
            asm volatile("pminw %0, %1, %2" : "=r"(low) : "r"(sum), "r"(difference));
            asm volatile("pextlw %0, %1, %2" : "=r"(packed) : "r"(high), "r"(low));
            asm volatile("pcpyld %0, %1, %2" : "=r"(packed) : "r"(packed), "r"(high));
            asm volatile("psllw %0, %1, 3" : "=r"(packed) : "r"(packed));
            total += checksum(packed);
            // Feeding the results back would double the lanes every other
            // round until they wrapped to nothing.
            gVectorsOut[i] = packed;
            gVectorsOut[i + 1] = high;
        }
    }
    return total;
}

static unsigned int vuWork(void)
{
    unsigned int total = 0;
    int round, i;
    for (round = 0; round < ROUNDS; ++round) {
        for (i = 0; i + 1 < ARRAY; i += 2) {
            register u128 result;
            asm volatile("qmtc2 %0, $vf1" : : "r"(gVectors[i]));
            asm volatile("qmtc2 %0, $vf2" : : "r"(gVectors[i + 1]));
            asm volatile("vmul.xyzw $vf3, $vf1, $vf2");
            asm volatile("vadd.xyzw $vf4, $vf3, $vf1");
            asm volatile("vsub.xyzw $vf5, $vf4, $vf2");
            asm volatile("vmula.xyzw $ACC, $vf1, $vf2");
            asm volatile("vmadd.xyzw $vf6, $vf5, $vf3");
            asm volatile("vmini.xyzw $vf7, $vf6, $vf4");
            asm volatile("vmax.xyzw $vf8, $vf7, $vf5");
            asm volatile("qmfc2 %0, $vf8" : "=r"(result));
            total += checksum(result);
        }
    }
    return total;
}

static unsigned int fpuWork(void)
{
    float total = 0.0f;
    int round, i;
    for (round = 0; round < ROUNDS; ++round) {
        for (i = 0; i + 1 < ARRAY; i += 2) {
            float x = gFloats[i];
            float y = gFloats[i + 1];
            float product, sum, root, smaller;
            asm volatile("mul.s %0, %1, %2" : "=f"(product) : "f"(x), "f"(y));
            asm volatile("adda.s %1, %2\n"
                         "madd.s %0, %1, %2" : "=f"(sum) : "f"(x), "f"(y));
            asm volatile("sqrt.s %0, %1" : "=f"(root) : "f"(product));
            asm volatile("min.s %0, %1, %2" : "=f"(smaller) : "f"(sum), "f"(root));
            total += smaller;
        }
    }
    return (unsigned int)total;
}

static unsigned int integerWork(void)
{
    unsigned int total = 0;
    int round, i;
    for (round = 0; round < ROUNDS; ++round) {
        for (i = 0; i < ARRAY; ++i) {
            unsigned int v = gInts[i] * 1103515245u + 12345u;
            gInts[i] = v;
            total ^= v + (v >> 13);
            total = (total << 1) | (total >> 31);
        }
    }
    return total;
}

static unsigned int floatWork(void)
{
    float accumulator = 0.0f;
    int round, i;
    for (round = 0; round < ROUNDS; ++round) {
        for (i = 0; i + 3 < ARRAY; i += 4) {
            float x = gFloats[i] * 0.5f + 0.5f;
            float y = gFloats[i + 1] * 0.25f - 0.25f;
            float z = gFloats[i + 2] * 0.5f + x * y;
            float w = gFloats[i + 3] * 0.5f - x * z;
            gFloats[i] = x;
            gFloats[i + 1] = y;
            gFloats[i + 2] = z;
            gFloats[i + 3] = w;
            accumulator += x + y + z + w;
        }
    }
    return (unsigned int)accumulator;
}

static unsigned int copyWork(void)
{
    unsigned int total = 0;
    int round, i;
    for (round = 0; round < ROUNDS; ++round) {
        memcpy(gDestination, gSource, ARRAY);
        for (i = 0; i < ARRAY; i += 64) {
            total += gDestination[i];
        }
        gSource[round & (ARRAY - 1)] = (unsigned char)total;
    }
    return total;
}

static unsigned int chaseWork(void)
{
    unsigned int total = 0;
    int round;
    for (round = 0; round < ROUNDS; ++round) {
        struct Node *node = &gNodes[0];
        while (node != 0) {
            total += node->value;
            node->value = total;
            node = node->next;
        }
    }
    return total;
}

static unsigned int fibonacci(unsigned int n)
{
    return n < 2 ? n : fibonacci(n - 1) + fibonacci(n - 2);
}

static unsigned int callWork(void)
{
    unsigned int total = 0;
    int round;
    for (round = 0; round < ROUNDS; ++round) {
        total += fibonacci(18);
    }
    return total;
}

static void seed(void)
{
    int i;
    for (i = 0; i < ARRAY; ++i) {
        gInts[i] = (unsigned int)(i * 2654435761u);
        gFloats[i] = (float)(i & 255) * 0.125f;
        gSource[i] = (unsigned char)(i * 37);
        gNodes[i].value = (unsigned int)i;
        gNodes[i].next = (i + 1 < ARRAY) ? &gNodes[i + 1] : 0;
        // Full mantissas: lanes that are powers of two leave the low bits
        // zero, and enough of those added together come to nothing.
        gVectors[i] = lanes((float)(i & 15) + 0.318309f, (float)(i & 7) - 2.718281f,
                            (float)(i & 31) * 0.161803f, (float)(i & 3) + 1.414213f);
    }
}

#define ALL_SECTIONS 0xFFFFFFFFu

// The driver writes which section to run before the guest starts, so one
// section can be timed on its own without the others in the way. Initialised,
// because startup clears the zeroed data after the driver has written it.
volatile unsigned int gSection = ALL_SECTIONS;

static int selected(unsigned int section)
{
    return gSection == ALL_SECTIONS || gSection == section;
}

int main(void)
{
    printf("-- TEST BEGIN\n");
    seed();
    // Plain conditionals rather than a switch: a switch becomes a jump table,
    // and code only an unrecovered table reaches is code nothing translates.
    if (selected(1)) printf("integer %08x\n", integerWork());
    if (selected(2)) printf("float %08x\n", floatWork());
    if (selected(3)) printf("copy %08x\n", copyWork());
    if (selected(4)) printf("chase %08x\n", chaseWork());
    if (selected(5)) printf("call %08x\n", callWork());
    if (selected(6)) printf("mmi %08x\n", mmiWork());
    if (selected(7)) printf("vu %08x\n", vuWork());
    if (selected(8)) printf("fpu %08x\n", fpuWork());
    printf("-- TEST END\n");
    return 0;
}
