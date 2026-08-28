#include <kerr.h>
#include <stdio.h>
#include <tamtypes.h>
#include <thevent.h>

static int isHandle(int id)
{
    return id > 0 && (id & 1) != 0;
}

static void lifecycle(void)
{
    iop_event_t event;
    iop_event_info_t info;
    int id;

    event.attr = 0;
    event.option = 0;
    event.bits = 0x0F;
    id = CreateEventFlag(&event);
    printf("create: handle %d\n", isHandle(id));

    if (ReferEventFlagStatus(id, &info) == 0) {
        printf("refer: init %08x, current %08x, waiting %d\n", info.initBits, info.currBits,
               info.numThreads);
    }

    printf("delete: %d\n", DeleteEventFlag(id));
    printf("delete twice: %d\n", DeleteEventFlag(id));
    printf("unknown: %d %d %d\n", SetEventFlag(1, 1), ClearEventFlag(1, 1),
           ReferEventFlagStatus(1, &info));
}

static void bits(void)
{
    iop_event_t event;
    iop_event_info_t info;
    int id;

    event.attr = 0;
    event.option = 0;
    event.bits = 0;
    id = CreateEventFlag(&event);

    // Set unions, clear intersects with the complement of its argument.
    SetEventFlag(id, 0x0000000F);
    SetEventFlag(id, 0x000000F0);
    ReferEventFlagStatus(id, &info);
    printf("set unions: %08x\n", info.currBits);

    ClearEventFlag(id, ~0x000000F0u);
    ReferEventFlagStatus(id, &info);
    printf("clear keeps its mask: %08x\n", info.currBits);

    DeleteEventFlag(id);
}

static void waiting(void)
{
    iop_event_t event;
    int id;
    u32 result;

    event.attr = 0;
    event.option = 0;
    event.bits = 0x0000000C;
    id = CreateEventFlag(&event);

    result = 0;
    printf("poll or: %d %08x\n", PollEventFlag(id, 0x00000004, WEF_OR, &result), result);
    result = 0;
    printf("poll and met: %d %08x\n", PollEventFlag(id, 0x0000000C, WEF_AND, &result), result);
    result = 0;
    printf("poll and unmet: %d\n", PollEventFlag(id, 0x0000000F, WEF_AND, &result));
    result = 0;
    printf("poll or unmet: %d\n", PollEventFlag(id, 0x00000030, WEF_OR, &result));

    // A satisfied wait with WEF_CLEAR takes the bits away on its way out.
    result = 0;
    printf("poll clear: %d %08x\n", PollEventFlag(id, 0x00000004, WEF_OR | WEF_CLEAR, &result),
           result);
    result = 0;
    printf("cleared: %d\n", PollEventFlag(id, 0x00000004, WEF_OR, &result));

    // A pattern of zero can never be satisfied, and the mode bits outside
    // WEF_OR and WEF_CLEAR are not a mode this accepts.
    printf("zero pattern: %d\n", PollEventFlag(id, 0, WEF_OR, &result));
    printf("bad mode: %d\n", PollEventFlag(id, 1, 0x40, &result));

    DeleteEventFlag(id);
}

int _start(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    printf("-- TEST BEGIN\n");
    lifecycle();
    bits();
    waiting();
    printf("-- TEST END\n");
    return 0;
}
