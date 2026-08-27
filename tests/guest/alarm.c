#include <kernel.h>
#include <stdio.h>

// GetAlarmCount is syscall 82 and ps2sdk declares no wrapper for it.
static int getAlarmCount(void)
{
    register int number asm("$3") = 82;
    register int result asm("$2");
    asm volatile("syscall" : "=r"(result) : "r"(number) : "memory");
    return result;
}

static volatile int gFired;
static volatile s32 gFiredId;
static volatile void *gFiredCommon;

static void onAlarm(s32 id, u16 time, void *common)
{
    (void)time;
    gFiredId = id;
    gFiredCommon = common;
    gFired = 1;
}

static int spinUntilFired(void)
{
    int spins;
    for (spins = 0; spins < 4000000; ++spins) {
        if (gFired) {
            return 1;
        }
    }
    return 0;
}

int main(void)
{
    s32 first, second, elapsed;
    int marker = 0x5A;

    printf("-- TEST BEGIN\n");

    printf("empty: count %d\n", (int)getAlarmCount());

    first = _SetAlarm(200, onAlarm, &marker);
    printf("set: id %d, count %d\n", (int)first, (int)getAlarmCount());

    second = _SetAlarm(400, onAlarm, &marker);
    printf("set again: id %d, count %d\n", (int)second, (int)getAlarmCount());

    elapsed = _ReleaseAlarm(second);
    printf("release: elapsed %d, count %d\n", elapsed >= 0 ? 1 : 0, (int)getAlarmCount());

    printf("release again: %d\n", (int)_ReleaseAlarm(second));
    printf("release bad id: %d\n", (int)_ReleaseAlarm(60));

    gFired = 0;
    if (spinUntilFired()) {
        printf("fired: id %d, common matches %d\n", (int)gFiredId,
               gFiredCommon == (void *)&marker ? 1 : 0);
    } else {
        printf("fired: never\n");
    }
    printf("after firing: count %d\n", (int)getAlarmCount());

    printf("-- TEST END\n");
    return 0;
}
