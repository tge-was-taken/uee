#include <stdio.h>
#include <sysclib.h>
#include <sysmem.h>
#include <tamtypes.h>

static void sizes(void)
{
    u32 total = QueryMemSize();
    u32 free = QueryTotalFreeMemSize();
    u32 largest = QueryMaxFreeMemSize();

    printf("memory size: %08x\n", total);
    printf("free fits: %d\n", free <= total);
    printf("largest fits: %d\n", largest <= free);
}

static void strategies(void)
{
    void *first;
    void *last;
    void *again;

    // ALLOC_FIRST takes the lowest block that fits and ALLOC_LAST the highest,
    // so with a free heap the two land at different ends.
    first = AllocSysMemory(ALLOC_FIRST, 4096, NULL);
    last = AllocSysMemory(ALLOC_LAST, 4096, NULL);
    printf("both allocated: %d\n", first != NULL && last != NULL);
    printf("ends differ: %d\n", first != last && (u32)first < (u32)last);
    printf("aligned: %d %d\n", ((u32)first & 0xFF) == 0, ((u32)last & 0xFF) == 0);

    printf("block size: %d\n", QueryBlockSize(first) >= 4096);
    printf("block top: %d\n", QueryBlockTopAddress(first) == first);

    printf("free: %d %d\n", FreeSysMemory(first), FreeSysMemory(last));

    // The lowest block is free again, so the same request comes back to it.
    again = AllocSysMemory(ALLOC_FIRST, 4096, NULL);
    printf("reused: %d\n", again == first);
    FreeSysMemory(again);
}

static void accounting(void)
{
    u32 before = QueryTotalFreeMemSize();
    void *block = AllocSysMemory(ALLOC_FIRST, 8192, NULL);
    u32 during = QueryTotalFreeMemSize();

    printf("took at least what was asked: %d\n", block != NULL && before - during >= 8192);
    FreeSysMemory(block);
    printf("gave it back: %d\n", QueryTotalFreeMemSize() == before);
}

static void rejections(void)
{
    printf("too large: %d\n", AllocSysMemory(ALLOC_FIRST, 0x7FFFFFFF, NULL) == NULL);
    printf("free null: %d\n", FreeSysMemory(NULL));
    printf("free unowned: %d\n", FreeSysMemory((void *)0x00000010));
}

int _start(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    printf("-- TEST BEGIN\n");
    sizes();
    strategies();
    accounting();
    rejections();
    printf("-- TEST END\n");
    return 0;
}
