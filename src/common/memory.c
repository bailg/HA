// memory.c — Memory allocation helpers (safe_malloc, safe_strdup)
#include "common/memory.h"

// Allocate memory, abort on failure
void* safe_malloc(size_t size) {
    void *ptr = malloc(size);
    if (!ptr) {
        LOG_FATAL("Out of memory");
    }
    return ptr;
}

// Duplicate string with safe_malloc, returns NULL if s is NULL
char* safe_strdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char *dup = safe_malloc(len);
    memcpy(dup, s, len);
    return dup;
}