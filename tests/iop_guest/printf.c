#include <stdio.h>
#include <sysclib.h>
#include <tamtypes.h>

// The host answers stdio.printf by handing each directive to its own printf, so
// what this checks is that the directive is carved out of the format correctly
// and the argument is fetched from the right place.
static void conversions(void)
{
    printf("signed: %d %d %d\n", 0, -1, -2147483647 - 1);
    printf("unsigned: %u %u\n", 0u, 4294967295u);
    printf("hex: %x %X %o\n", 0xDEADBEEFu, 0xDEADBEEFu, 8u);
    printf("char and text: %c%c %s\n", 'o', 'k', "string");
    printf("percent: 100%%\n");
}

static void padding(void)
{
    printf("width: [%5d] [%-5d] [%05d]\n", 42, 42, 42);
    printf("sign: [%+d] [% d]\n", 42, 42);
    printf("hex width: [%08x] [%#x]\n", 0x1234u, 0x1234u);
    printf("text width: [%8s] [%-8s] [%.3s]\n", "abc", "abc", "abcdef");
}

// The IOP has no type wider than the host's int, so a length modifier cannot
// change what the conversion reads.
static void lengths(void)
{
    printf("long: %ld %lx %lu\n", -7, 0xABCu, 7u);
    printf("short: %hd %hx\n", -7, 0xABCu);
}

// o32 passes four arguments in registers and the rest on the stack, so a call
// with more than four has to keep counting past the register window.
static void spilled(void)
{
    printf("eight: %d %d %d %d %d %d %d %d\n", 1, 2, 3, 4, 5, 6, 7, 8);
    printf("mixed: %s %d %s %d %s %d\n", "a", 1, "b", 2, "c", 3);
}

static void library(void)
{
    char source[] = "copied through sysclib";
    char destination[32];

    memcpy(destination, source, strlen(source) + 1);
    printf("memcpy: %s\n", destination);
    printf("strlen: %d %d\n", strlen(source), strlen(""));
}

int _start(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    printf("-- TEST BEGIN\n");
    conversions();
    padding();
    lengths();
    spilled();
    library();
    printf("-- TEST END\n");
    return 0;
}
