#include <intrman.h>
#include <kerr.h>
#include <stdio.h>
#include <tamtypes.h>

#define IRQ_TIMER0 4
// INTRMANI: RegisterIntrHandler @ 0x148 takes the 32 CPU lines and the DMA
// lines above them, so the last one it accepts is 45.
#define IRQ_LAST   45

static int handler(void *arg)
{
    (void)arg;
    return 1;
}

// Every threadman entry refuses to run unless this reads zero, so a module's
// whole service surface depends on it answering for the context it is in.
static void context(void)
{
    printf("thread context: %d\n", QueryIntrContext());
}

// Suspend hands back the state that resume takes, so the pair nests and the
// second suspend has to observe what the first left rather than the state the
// thread started in.
static void suspending(void)
{
    int outer = -1;
    int inner = -1;
    int again = -1;

    CpuSuspendIntr(&outer);
    CpuSuspendIntr(&inner);
    printf("state written: %d %d\n", outer != -1, inner != -1);
    printf("nesting seen: %d\n", outer != inner);

    CpuResumeIntr(inner);
    CpuResumeIntr(outer);

    // Back where it started, so a fresh suspend has to match the first one.
    CpuSuspendIntr(&again);
    printf("restored: %d\n", again == outer);
    CpuResumeIntr(again);
}

static void handlers(void)
{
    int registered = RegisterIntrHandler(IRQ_TIMER0, 1, handler, NULL);
    int twice = RegisterIntrHandler(IRQ_TIMER0, 1, handler, NULL);

    printf("register: %d\n", registered >= 0);
    printf("register twice refused: %d\n", twice < 0);
    printf("release: %d\n", ReleaseIntrHandler(IRQ_TIMER0) >= 0);
    printf("release twice refused: %d\n", ReleaseIntrHandler(IRQ_TIMER0) < 0);
}

static void rejections(void)
{
    printf("line past the end: %d\n", RegisterIntrHandler(IRQ_LAST + 1, 1, handler, NULL) < 0);
    printf("negative line: %d\n", RegisterIntrHandler(-1, 1, handler, NULL) < 0);
    printf("no handler: %d\n", RegisterIntrHandler(IRQ_TIMER0, 1, NULL, NULL) < 0);
}

int _start(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    printf("-- TEST BEGIN\n");
    context();
    suspending();
    handlers();
    rejections();
    printf("-- TEST END\n");
    return 0;
}
