#include <stdio.h>

// The guest stack bounds how deep any program can go, and a small frame is what
// reaches the bottom of it in the most calls. Translated code spends a host
// frame per guest call, so this is the deepest a host stack is ever asked for.
#define GUEST_STACK_BYTES (128 * 1024)
#define HEADROOM_BYTES (8 * 1024)

static char* entry;
static int deepest;

static int descend(int depth) {
    volatile int marker = depth;
    if (entry - (char*)&marker >= GUEST_STACK_BYTES - HEADROOM_BYTES) {
        deepest = depth;
        return marker;
    }
    return descend(depth + 1) + marker;
}

int main(void) {
    volatile int here = 0;
    entry = (char*)&here;
    descend(0);
    printf("descended %d calls in %d bytes\n", deepest,
           GUEST_STACK_BYTES - HEADROOM_BYTES);
    return 0;
}
