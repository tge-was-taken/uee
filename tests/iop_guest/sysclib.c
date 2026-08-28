#include <ctype.h>
#include <stdio.h>
#include <sysclib.h>
#include <tamtypes.h>

// The table carries a punctuation bit on space and no hex-digit bit on the
// digits, neither of which a C locale would give you.
static void ctype(void)
{
    static const char probe[] = {0, '\t', ' ', '0', '9', 'A', 'F', 'G', 'a', 'f', 'g', '~', 127};
    int i;

    printf("ctype:");
    for (i = 0; i < (int)(sizeof(probe) / sizeof(probe[0])); i++) {
        printf(" %02x", look_ctype_table(probe[i]));
    }
    printf("\n");
}

// memcmp reports only the sign of the difference; strncmp beside it reports the
// difference itself, so a pair far apart in the alphabet tells them apart.
static void compare(void)
{
    printf("memcmp: %d %d %d\n", memcmp("abc", "abz", 3), memcmp("abz", "abc", 3),
           memcmp("abc", "abc", 3));
    printf("strncmp: %d %d %d\n", strncmp("abc", "abz", 3), strncmp("abz", "abc", 3),
           strncmp("abc", "abc", 3));
    printf("strncmp bounded: %d\n", strncmp("abc", "abz", 2));
}

static void copy(void)
{
    char buffer[16];

    memset(buffer, '#', sizeof(buffer));
    buffer[15] = 0;
    printf("memset: %s\n", buffer);

    bzero(buffer, 8);
    printf("bzero: %d\n", (int)strlen(buffer));

    strcpy(buffer, "one");
    strcat(buffer, "-two");
    printf("strcat: %s %d\n", buffer, (int)strlen(buffer));

    memset(buffer, '.', sizeof(buffer));
    strncpy(buffer, "ab", 6);
    printf("strncpy: %02x %02x %02x %02x %02x %02x %02x\n", buffer[0], buffer[1], buffer[2],
           buffer[3], buffer[4], buffer[5], buffer[6]);
}

static void numbers(void)
{
    char *rest;
    long value = strtol("123abc", &rest, 10);

    printf("strtol: %d rest %s\n", (int)value, rest);
    printf("strtol bases: %d %d %d\n", (int)strtol("-42", 0, 10), (int)strtol(" 0x1f", 0, 0),
           (int)strtol("0777", 0, 0));
}

static void text(void)
{
    char buffer[32];
    int written = sprintf(buffer, "%s=%d", "n", 7);

    printf("sprintf: %s %d\n", buffer, written);
}

int _start(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    printf("-- TEST BEGIN\n");
    ctype();
    compare();
    copy();
    numbers();
    text();
    printf("-- TEST END\n");
    return 0;
}
