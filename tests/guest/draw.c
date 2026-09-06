#include <kernel.h>
#include <stdio.h>
#include <tamtypes.h>

#include "syscall.h"

#define GS_PMODE (*(volatile u64 *)0x12000000)
#define GS_SMODE2 (*(volatile u64 *)0x12000020)
#define GS_DISPFB1 (*(volatile u64 *)0x12000070)
#define GS_DISPLAY1 (*(volatile u64 *)0x12000080)
#define GS_BGCOLOR (*(volatile u64 *)0x120000E0)
#define GS_CSR (*(volatile u64 *)0x12001000)

#define D2_CHCR (*(volatile u32 *)0x1000A000)
#define D2_MADR (*(volatile u32 *)0x1000A010)
#define D2_QWC (*(volatile u32 *)0x1000A020)

#define CSR_FINISH 2
#define CSR_VSINT 8

#define WIDTH 640
#define HEIGHT 448

enum {
    REG_PRIM = 0x00,
    REG_RGBAQ = 0x01,
    REG_XYZ2 = 0x05,
    REG_XYOFFSET_1 = 0x18,
    REG_PRMODECONT = 0x1A,
    REG_SCISSOR_1 = 0x40,
    REG_COLCLAMP = 0x46,
    REG_TEST_1 = 0x47,
    REG_FRAME_1 = 0x4C,
    REG_ZBUF_1 = 0x4E,
    REG_FINISH = 0x61,
};

static u64 gPacket[256] __attribute__((aligned(16)));
static u32 gWords;

static void reset(void)
{
    gWords = 0;
}

static void tag(u32 loops, u32 primitive, int usePrimitive)
{
    gPacket[gWords++] = (u64)loops | (1ULL << 15) | ((u64)(usePrimitive ? 1 : 0) << 46) |
                        ((u64)primitive << 47) | (1ULL << 60);
    gPacket[gWords++] = 0xE;
}

static void reg(u32 address, u64 value)
{
    gPacket[gWords++] = value;
    gPacket[gWords++] = address;
}

static void vertex(u32 x, u32 y, u32 colour)
{
    gPacket[gWords++] = colour;
    gPacket[gWords++] = REG_RGBAQ;
    gPacket[gWords++] = ((u64)x << 4) | ((u64)y << 20);
    gPacket[gWords++] = REG_XYZ2;
}

static void send(void)
{
    FlushCache(0);
    D2_QWC = gWords / 2;
    D2_MADR = (u32)gPacket;
    D2_CHCR = 0x101;
    while ((D2_CHCR & 0x100) != 0) {
        continue;
    }
}

static int waitFor(u64 bit)
{
    int spins;
    GS_CSR = bit;
    for (spins = 0; spins < 40000000; ++spins) {
        if ((GS_CSR & bit) != 0) {
            return 1;
        }
    }
    return 0;
}

static void setupDisplay(void)
{
    GS_PMODE = 0xFF25;
    GS_SMODE2 = 0;
    GS_DISPFB1 = ((u64)(WIDTH / 64) << 9);
    GS_DISPLAY1 = 636ULL | (50ULL << 12) | (3ULL << 23) | ((u64)(WIDTH * 4 - 1) << 32) |
                  ((u64)(HEIGHT - 1) << 44);
    GS_BGCOLOR = 0;
}

static void setupDrawing(void)
{
    reset();
    tag(7, 0, 0);
    reg(REG_FRAME_1, (u64)(WIDTH / 64) << 16);
    reg(REG_ZBUF_1, 1ULL << 32);
    reg(REG_XYOFFSET_1, 0);
    reg(REG_SCISSOR_1, (u64)(WIDTH - 1) << 16 | ((u64)(HEIGHT - 1) << 48));
    reg(REG_PRMODECONT, 1);
    reg(REG_COLCLAMP, 1);
    reg(REG_TEST_1, (1ULL << 16) | (1ULL << 17));
    send();
}

static void drawFrame(u32 shade)
{
    reset();

    tag(3, 6, 1);
    gPacket[gWords++] = 0x000020;
    gPacket[gWords++] = REG_RGBAQ;
    gPacket[gWords++] = 0;
    gPacket[gWords++] = REG_XYZ2;
    gPacket[gWords++] = ((u64)WIDTH << 4) | ((u64)HEIGHT << 20);
    gPacket[gWords++] = REG_XYZ2;

    tag(6, 3 | (1 << 3), 1);
    vertex(320, 60, 0x0000FF | (u64)shade << 8);
    vertex(120, 380, 0x00FF00);
    vertex(520, 380, 0xFF0000);

    tag(1, 0, 0);
    reg(REG_FINISH, 0);
    send();
}

int main(void)
{
    int frame;

    printf("-- TEST BEGIN\n");

    setupDisplay();
    setupDrawing();

    for (frame = 0; frame < 3; ++frame) {
        drawFrame((u32)frame * 64);
        printf("frame %d finished: %d\n", frame, waitFor(CSR_FINISH));
        printf("frame %d displayed: %d\n", frame, waitFor(CSR_VSINT));
    }

    printf("-- TEST END\n");
    return 0;
}
