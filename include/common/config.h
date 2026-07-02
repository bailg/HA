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

/* 校验: 必需键存在 */
int config_validate_required(const Config *cfg, const char *key);
/* 校验: int 值在 [min, max] 范围内, 不满足则 LOG_FATAL */
int config_validate_int_range(const Config *cfg, const char *key, int min, int max, int default_val);