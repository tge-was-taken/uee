#include <stdlib.h>

// minicoro allocates with calloc, which is free on a fresh mapping and a
// megabyte of memset on a recycled one.
#define MCO_ALLOC(size) malloc(size)
#define MCO_DEALLOC(ptr, size) free(ptr)

#define MINICORO_IMPL
#include <minicoro.h>
