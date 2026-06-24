#include "common/memory.h"
void* safe_malloc(size_t size) {
    void *ptr = malloc(size);
    if (!ptr) {
        LOG_FATAL("Out of memory");
    }
    return ptr;
}
char* safe_strdup(const char *s) {
    char *dup = strdup(s);
    if (!dup) {
        LOG_FATAL("Out of memory");
    }
    return dup;
}
