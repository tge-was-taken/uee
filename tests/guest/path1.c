#include <kernel.h>
#include <stdio.h>
#include <tamtypes.h>

#include "syscall.h"

#define VIF1_FIFO ((volatile u128 *)0x10005000)
#define VU1_MICRO ((volatile u32 *)0x11008000)
#define VU1_MEM ((volatile u32 *)0x1100C000)
#define GS_SIGLBLID (*(volatile u64 *)0x12001080)
#define D1_CHCR (*(volatile u32 *)0x10009000)
#define D1_MADR (*(volatile u32 *)0x10009010)
#define D1_QWC (*(volatile u32 *)0x10009020)

#define MARKER 0x777
#define LABEL_ID 0xCAFEBABE

// dvp-as of: iaddiu vi02,vi00,1911 / iaddiu vi01,vi00,4 / nop /
// iswr.x vi02,(vi00) / xgkick vi01 / nop[e] / nop, each paired with an upper nop.
static const u32 kMicro[14] = {
    0x10020777, 0x000002FF,
    0x10010004, 0x000002FF,
    0x8000033C, 0x000002FF,
    0x810203FF, 0x000002FF,
    0x80000EFC, 0x000002FF,
    0x8000033C, 0x400002FF,
    0x8000033C, 0x000002FF,
};

static u32 gPacket[32] __attribute__((aligned(16)));

static u32 buildPacket(void)
{
    u32 n = 0;
    int i;

    gPacket[n++] = 0x01000101;
    gPacket[n++] = 0x4A000000 | (7 << 16);
    for (i = 0; i < 14; ++i) {
        gPacket[n++] = kMicro[i];
    }

    gPacket[n++] = 0x6C000000 | (2 << 16) | 4;
    gPacket[n++] = 0x00008001;
    gPacket[n++] = 0x10000000;
    gPacket[n++] = 0x0000000E;
    gPacket[n++] = 0x00000000;
    gPacket[n++] = LABEL_ID;
    gPacket[n++] = 0xFFFFFFFF;
    gPacket[n++] = 0x00000062;
    gPacket[n++] = 0x00000000;

    gPacket[n++] = 0x14000000;
    while ((n & 3) != 0) {
        gPacket[n++] = 0;
    }
    return n / 4;
}

static u32 readVpuStat(void)
{
    u32 stat;
    asm volatile("cfc2 %0, $29" : "=r"(stat));
    return stat;
}

static void waitForVu1(void)
{
    int spins;
    for (spins = 0; spins < 1000000; ++spins) {
        if ((readVpuStat() & (1 << 8)) == 0) {
            return;
        }
    }
    printf("timed out waiting for vu1\n");
}

static void reset(void)
{
    int i;
    for (i = 0; i < 14; ++i) {
        VU1_MICRO[i] = 0;
    }
    VU1_MEM[0] = 0;
    GS_SIGLBLID = 0;
}

static int microUploaded(void)
{
    int i;
    for (i = 0; i < 14; ++i) {
        if (VU1_MICRO[i] != kMicro[i]) {
            return 0;
        }
    }
    return 1;
}

static void report(const char *how)
{
    printf("%s micro uploaded: %d\n", how, microUploaded());
    printf("%s mscal marker: %08x\n", how, VU1_MEM[0]);
    printf("%s xgkick label: %08x\n", how, (u32)(GS_SIGLBLID >> 32));
}

static void sendByFifo(u32 quadwords)
{
    const u128 *source = (const u128 *)gPacket;
    u32 i;
    for (i = 0; i < quadwords; ++i) {
        VIF1_FIFO[0] = source[i];
    }
}

static void sendByDma(u32 quadwords)
{
    FlushCache(0);
    D1_QWC = quadwords;
    D1_MADR = (u32)gPacket;
    D1_CHCR = 0x101;
    while ((D1_CHCR & 0x100) != 0) {
        continue;
    }
}

int main(void)
{
    const u32 quadwords = buildPacket();

    printf("-- TEST BEGIN\n");

    reset();
    sendByFifo(quadwords);
    waitForVu1();
    report("fifo");

    reset();
    sendByDma(quadwords);
    waitForVu1();
    report("dma");

    printf("-- TEST END\n");
    return 0;
}
