#pragma once
#include "logging.h"

#define MAX_CONFIG_LINE 256
#define MAX_CONFIG_ENTRIES 64

typedef struct {
    char key[64];
    char value[192];
} ConfigEntry;

typedef struct {
    ConfigEntry entries[MAX_CONFIG_ENTRIES];
    int count;
} Config;

int config_load(Config *cfg, const char *filename);
const char* config_get(const Config *cfg, const char *key);
int config_get_int(const Config *cfg, const char *key, int default_val);