#include <kerr.h>
#include <stdio.h>
#include <sysclib.h>
#include <tamtypes.h>
#include <thsemap.h>

// An id packs the control block address, a rolling counter and a set low bit,
// so it is always odd and never a small index the way an EE id is.
static int isHandle(int id)
{
    return id > 0 && (id & 1) != 0;
}

static void lifecycle(void)
{
    iop_sema_t sema;
    iop_sema_info_t info;
    int id, other;

    sema.attr = 0;
    sema.option = 0;
    sema.initial = 1;
    sema.max = 4;
    id = CreateSema(&sema);
    printf("create: handle %d\n", isHandle(id));

    other = CreateSema(&sema);
    printf("distinct: %d\n", isHandle(other) && other != id);

    if (ReferSemaStatus(id, &info) == 0) {
        printf("refer: initial %d, max %d, current %d, waiting %d\n", info.initial, info.max,
               info.current, info.numWaitThreads);
    }

    printf("delete: %d %d\n", DeleteSema(other), DeleteSema(id));
    printf("delete twice: %d\n", DeleteSema(id));
    printf("refer deleted: %d\n", ReferSemaStatus(id, &info));
}

static void counting(void)
{
    iop_sema_t sema;
    iop_sema_info_t info;
    int id;

    sema.attr = 0;
    sema.option = 0;
    sema.initial = 0;
    sema.max = 2;
    id = CreateSema(&sema);

    printf("poll empty: %d\n", PollSema(id));
    printf("signal: %d %d\n", SignalSema(id), SignalSema(id));

    // max is a ceiling on the count, and a signal past it is refused rather
    // than clamped.
    printf("signal past max: %d\n", SignalSema(id));
    ReferSemaStatus(id, &info);
    printf("still at max: %d\n", info.current);

    printf("wait: %d %d\n", WaitSema(id), WaitSema(id));
    printf("poll drained: %d\n", PollSema(id));
    ReferSemaStatus(id, &info);
    printf("count: %d\n", info.current);

    DeleteSema(id);
}

static void rejections(void)
{
    iop_sema_t sema;
    iop_sema_info_t info;
    int stale;

    sema.attr = 0;
    sema.option = 0;
    sema.initial = 0;
    sema.max = 1;

    // Only the two queueing bits are defined; anything else is refused.
    sema.attr = 0x4000;
    printf("bad attr: %d\n", CreateSema(&sema));
    sema.attr = SA_THPRI;
    printf("priority attr: %d\n", isHandle(CreateSema(&sema)));

    // The id carries a counter as well as the slot, so a handle to a deleted
    // semaphore fails the counter half of the check that a wild id never reaches.
    sema.attr = 0;
    stale = CreateSema(&sema);
    DeleteSema(stale);
    printf("stale handle: %d %d %d\n", SignalSema(stale), WaitSema(stale), PollSema(stale));

    printf("unknown semid: %d %d %d %d\n", SignalSema(1), WaitSema(1), PollSema(1),
           ReferSemaStatus(1, &info));
    printf("delete unknown: %d\n", DeleteSema(1));
}

int _start(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    printf("-- TEST BEGIN\n");
    lifecycle();
    counting();
    rejections();
    printf("-- TEST END\n");
    return 0;
}
