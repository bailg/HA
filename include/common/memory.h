// memory.h — Safe memory allocation and deallocation utilities
#pragma once
#include <stdlib.h>
#include <string.h>
#include "logging.h"

// Safely free pointer and set to NULL
#define SAFE_FREE(ptr) do { if (ptr) { free(ptr); ptr = NULL; } } while(0)

// Allocate memory with NULL-check, exits on failure
void* safe_malloc(size_t size);
// Duplicate string with NULL-check, exits on failure
char* safe_strdup(const char *s);