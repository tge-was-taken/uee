#include <dmacman.h>
#include <stdio.h>
#include <tamtypes.h>

// SIO2MAN md5 ea22447abe3a4b56a26f3149316370a3 drives these: Sio2TransferSubmit
// @ 0x1c4 writes SEND3 and pushes the command a byte at a time,
// Sio2TransferStart @ 0x2f0 sets bit 0 of CTRL, and
// Sio2TransferCollectResults @ 0x310 reads the answer back a byte at a time.
#define SIO2_SEND3   ((volatile u32 *)0xBF808200)
#define SIO2_FIFO_IN ((volatile u8 *)0xBF808260)
#define SIO2_FIFO_OUT ((volatile u8 *)0xBF808264)
#define SIO2_CTRL    ((volatile u32 *)0xBF808268)
#define SIO2_RECV1   ((volatile u32 *)0xBF80826C)

// SIO2MAN: Sio2CtrlReset @ 0x18c, Sio2FifoReset @ 0x1a4.
#define CTRL_RESET   0x3BC
#define CTRL_FIFO    0xC
#define CTRL_START   0x1

// PADMAN md5 0fa9dbfe8794e172733189ed945aff45: PadSlotStateAdvance @ 0x4c4c
// gives up on a slot when this bit of RECV1 is set.
#define RECV1_FAILED (1 << 13)

#define PAD_POLL       0x42
#define PAD_CONFIG     0x43
#define PAD_MAIN_MODE  0x44

#define CARD_PORT      2
#define CARD_PROBE     0x11
#define CARD_SPECS     0x26
#define CARD_TERM_GET  0x28
#define CARD_WRITE_PAGE 0x22
#define CARD_READ_PAGE 0x23
#define CARD_WRITE     0x42
#define CARD_READ      0x43
#define CARD_ACK       0x2B
#define CARD_TERMINATOR 0x55
#define CARD_DATA      4

static u8 command[24];
static u8 answer[24];

// MC2_S1 md5 c493ecb797ddd06b4ebd7eee6aff1959 packs both lengths into one
// entry in Mc2Sio2QueueCommand @ 0x7c4, and the transfer clocks the longer one.
static void queue(int port, int sent, int clocked)
{
    SIO2_SEND3[0] = (u32)(port | (sent << 8) | (clocked << 18));
    SIO2_SEND3[1] = 0;
}

static void sendThroughFifo(int port, int sent, int clocked)
{
    int i;

    *SIO2_CTRL = CTRL_RESET;
    queue(port, sent, clocked);
    *SIO2_CTRL |= CTRL_FIFO;
    for (i = 0; i < sent; ++i) {
        *SIO2_FIFO_IN = command[i];
    }
    *SIO2_CTRL |= CTRL_START;
    for (i = 0; i < clocked; ++i) {
        answer[i] = *SIO2_FIFO_OUT;
    }
}

// PADMAN md5 0fa9dbfe8794e172733189ed945aff45 leaves the FIFOs to the DMA
// channels instead, which is the path Sio2TransferSubmit takes when the
// transfer carries an address.
static void sendThroughDma(int port, int length)
{
    *SIO2_CTRL = CTRL_RESET;
    queue(port, length, length);
    *SIO2_CTRL |= CTRL_FIFO;
    sceSetSliceDMA(IOP_DMAC_SIO2in, command, 2, 1, 1);
    sceStartDMA(IOP_DMAC_SIO2in);
    sceSetSliceDMA(IOP_DMAC_SIO2out, answer, 2, 1, 0);
    sceStartDMA(IOP_DMAC_SIO2out);
    *SIO2_CTRL |= CTRL_START;
}

static void ask(int port, int command1, int payload, int length)
{
    int i;

    for (i = 0; i < (int)sizeof(command); ++i) {
        command[i] = 0;
        answer[i] = 0;
    }
    command[0] = 1;
    command[1] = (u8)command1;
    command[3] = (u8)payload;
    sendThroughFifo(port, length, length);
}

static void header(const char *what)
{
    printf("%s: %02x %02x %02x\n", what, answer[0], answer[1], answer[2]);
}

static void polling(void)
{
    ask(0, PAD_POLL, 0, 5);
    header("poll");
    printf("poll buttons: %02x %02x\n", answer[3], answer[4]);
    printf("poll failed: %d\n", (*SIO2_RECV1 & RECV1_FAILED) != 0);
}

static void emptyPort(void)
{
    ask(1, PAD_POLL, 0, 5);
    header("empty");
    printf("empty failed: %d\n", (*SIO2_RECV1 & RECV1_FAILED) != 0);
}

// PADMAN: PadProbeXferStep4 @ 0x1414 expects 0xF3 from every frame it sends
// between entering and leaving config mode.
static void configMode(void)
{
    ask(0, PAD_CONFIG, 1, 9);
    header("entering config");
    ask(0, PAD_POLL, 0, 9);
    header("in config");
    ask(0, PAD_MAIN_MODE, 1, 9);
    ask(0, PAD_CONFIG, 0, 9);
    ask(0, PAD_POLL, 0, 9);
    header("analog");
    ask(0, PAD_CONFIG, 1, 9);
    ask(0, PAD_MAIN_MODE, 0, 9);
    ask(0, PAD_CONFIG, 0, 9);
    ask(0, PAD_POLL, 0, 5);
    header("digital again");
}

static void throughDma(void)
{
    int i;

    for (i = 0; i < (int)sizeof(command); ++i) {
        command[i] = 0;
        answer[i] = 0;
    }
    command[0] = 1;
    command[1] = PAD_POLL;
    sendThroughDma(0, 5);
    header("dma");
    printf("dma buttons: %02x %02x\n", answer[3], answer[4]);
}

static u8 checksum(const u8 *bytes, int length)
{
    u8 sum = 0;
    int i;

    for (i = 0; i < length; ++i) {
        sum = (u8)(sum ^ bytes[i]);
    }
    return sum;
}

static void card(int cmd, int sent, int clocked)
{
    int i;

    for (i = sent; i < (int)sizeof(command); ++i) {
        command[i] = 0;
    }
    for (i = 0; i < (int)sizeof(answer); ++i) {
        answer[i] = 0;
    }
    command[0] = 0x81;
    command[1] = (u8)cmd;
    sendThroughFifo(CARD_PORT, sent, clocked);
}

static void cardProbe(void)
{
    card(CARD_PROBE, 2, 4);
    printf("card ack: %d\n", answer[2] == CARD_ACK);
    printf("card terminator: %d\n", answer[3] == CARD_TERMINATOR);
}

// MC2_S1: Mc2GetSpecs @ 0x14cc rejects a card whose eight bytes of specs do not
// match the checksum that follows them.
static void cardSpecs(void)
{
    card(CARD_SPECS, 2, 13);
    printf("card page bytes: %d\n", answer[3] | (answer[4] << 8));
    printf("card block pages: %d\n", answer[5] | (answer[6] << 8));
    printf("card pages: %d\n", answer[7] | (answer[8] << 8) | (answer[9] << 16));
    printf("card specs checksum: %d\n", answer[11] == checksum(&answer[3], 8));
    printf("card specs terminator: %d\n", answer[12] == CARD_TERMINATOR);
}

static void cardTerminator(void)
{
    card(CARD_TERM_GET, 2, 5);
    printf("card terminator read: %02x\n", answer[3]);
}

static void setPage(int cmd, int page)
{
    command[2] = (u8)page;
    command[3] = (u8)(page >> 8);
    command[4] = (u8)(page >> 16);
    command[5] = (u8)(page >> 24);
    command[6] = checksum(&command[2], 4);
    card(cmd, 7, 9);
    command[2] = 0;
    command[3] = 0;
    command[4] = 0;
    command[5] = 0;
    command[6] = 0;
}

// MC2_S1: Mc2WritePage @ 0x111c sets the page and then sends the bytes with
// their checksum, and Mc2ReadPage @ 0xdb8 reads them back the same way.
static void cardPage(void)
{
    int i;

    setPage(CARD_WRITE_PAGE, 1);
    for (i = 0; i < 4; ++i) {
        command[3 + i] = (u8)(0x10 + i);
    }
    command[2] = 4;
    command[7] = checksum(&command[3], 4);
    card(CARD_WRITE, 8, 10);
    printf("card write acked: %d\n", answer[2] == CARD_ACK);

    setPage(CARD_READ_PAGE, 1);
    command[2] = 4;
    card(CARD_READ, 3, 10);
    printf("card read: %02x %02x %02x %02x\n", answer[CARD_DATA], answer[CARD_DATA + 1],
           answer[CARD_DATA + 2], answer[CARD_DATA + 3]);
    printf("card read checksum: %d\n", answer[CARD_DATA + 4] == checksum(&answer[CARD_DATA], 4));
    printf("card read terminator: %d\n", answer[9] == CARD_TERMINATOR);
}

int _start(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    printf("-- TEST BEGIN\n");
    polling();
    emptyPort();
    configMode();
    throughDma();
    cardProbe();
    cardSpecs();
    cardTerminator();
    cardPage();
    printf("-- TEST END\n");
    return 0;
}
