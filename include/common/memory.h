#ifndef MEMORY_H
#define MEMORY_H
#include <stdlib.h>
#include <string.h>
#include "logging.h"

#define SAFE_FREE(ptr) do { if (ptr) { free(ptr); ptr = NULL; } } while(0)

void* safe_malloc(size_t size);
char* safe_strdup(const char *s);

#endif /* MEMORY_H */