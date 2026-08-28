#include <kernel.h>
#include <stdio.h>
#include <string.h>

#include "syscall.h"

static volatile char gLog[16];
static volatile int gLogLength;

static void note(char c)
{
    if (gLogLength < (int)sizeof(gLog) - 1) {
        gLog[gLogLength++] = c;
        gLog[gLogLength] = 0;
    }
}

static volatile s32 gFiredId;
static volatile u16 gFiredTime;

static void handOverSema(s32 id, u16 time, void *common)
{
    gFiredId = id;
    gFiredTime = time;
    iSignalSema((s32)(long)common);
}

static void logLetter(s32 id, u16 time, void *common)
{
    (void)id;
    (void)time;
    note((char)(long)common);
}

static int spinUntil(volatile int *flag, int target)
{
    int spins;
    for (spins = 0; spins < 40000000; ++spins) {
        if (*flag >= target) {
            return 1;
        }
    }
    return 0;
}

int main(void)
{
    ee_sema_t sema;
    s32 slots[65];
    s32 wake, waited;
    int i;

    printf("-- TEST BEGIN\n");

    memset(&sema, 0, sizeof(sema));
    sema.init_count = 0;
    sema.max_count = 1;
    wake = CreateSema(&sema);

    // P3's DelayThread. The alarm callback runs in interrupt context and hands
    // the token straight to the blocked thread, which is why WaitSema comes
    // back with the semaphore id and the count stays where it was.
    _SetAlarm(200, handOverSema, (void *)(long)wake);
    waited = WaitSema(wake);
    printf("handover: wait %d, alarm id %d, time nonzero %d\n", waited == wake ? 1 : 0,
           (int)gFiredId, gFiredTime != 0 ? 1 : 0);
    printf("handover: alarms %d, count %d\n", getAlarmCount(),
           ReferSemaStatus(wake, &sema) >= 0 ? sema.count : -99);
    DeleteSema(wake);

    // The active list is kept sorted by expiry, so the order alarms are set in
    // does not decide the order they fire in.
    gLogLength = 0;
    _SetAlarm(300, logLetter, (void *)'c');
    _SetAlarm(100, logLetter, (void *)'a');
    _SetAlarm(200, logLetter, (void *)'b');
    printf("pending: %d\n", getAlarmCount());
    printf("order: %s\n", spinUntil(&gLogLength, 3) ? (char *)gLog : "timed out");

    // 64 slots, allocated from the lowest clear bit of the free mask.
    for (i = 0; i < 65; ++i) {
        slots[i] = _SetAlarm(0xF000, logLetter, (void *)'x');
    }
    printf("pool: first %d, last %d, overflow %d, count %d\n", (int)slots[0], (int)slots[63],
           (int)slots[64], getAlarmCount());

    printf("release mid: %d\n", _ReleaseAlarm(slots[7]) >= 0 ? 1 : 0);
    printf("reuse: %d\n", (int)_SetAlarm(0xF000, logLetter, (void *)'x'));

    for (i = 0; i < 64; ++i) {
        _ReleaseAlarm(slots[i]);
    }
    printf("drained: count %d\n", getAlarmCount());
    printf("release twice: %d\n", (int)_ReleaseAlarm(slots[0]));

    printf("-- TEST END\n");
    return 0;
}
