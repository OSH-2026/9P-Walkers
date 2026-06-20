#include "heap.h"

#include <stdlib.h>

void *heap_malloc_dbg(size_t size, const char *file, int line)
{
    (void)file;
    (void)line;
    return malloc(size);
}

void heap_free_dbg(void *ptr, const char *file, int line)
{
    (void)file;
    (void)line;
    free(ptr);
}
