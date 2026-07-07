// config.h — Configuration file parsing and validation
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

// Load configuration from file, returns 0 on success
int config_load(Config *cfg, const char *filename);
// Get string value for given key, returns NULL if not found
const char* config_get(const Config *cfg, const char *key);
// Get integer value for key with default fallback
int config_get_int(const Config *cfg, const char *key, int default_val);
// Validate that required key exists in config
int config_validate_required(const Config *cfg, const char *key);
// Validate integer value is within [min, max] range, LOG_FATAL on failure
int config_validate_int_range(const Config *cfg, const char *key, int min, int max, int default_val);