#include "common/config.h"
#include <string.h>
#include <stdlib.h>

int config_load(Config *cfg, const char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        LOG_ERROR("Cannot open config file: %s", filename);
        return -1;
    }
    char line[MAX_CONFIG_LINE];
    cfg->count = 0;
    while (fgets(line, sizeof(line), fp) && cfg->count < MAX_CONFIG_ENTRIES) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key_end = eq - 1;
        while (key_end >= p && (*key_end == ' ' || *key_end == '\t')) key_end--;
        *(key_end + 1) = '\0';
        char *val_start = eq + 1;
        while (*val_start == ' ' || *val_start == '\t') val_start++;
        char *val_end = val_start + strlen(val_start) - 1;
        while (val_end >= val_start && (*val_end == ' ' || *val_end == '\t' || *val_end == '\n' || *val_end == '\r')) val_end--;
        *(val_end + 1) = '\0';
        strncpy(cfg->entries[cfg->count].key, p, sizeof(cfg->entries[cfg->count].key) - 1);
        strncpy(cfg->entries[cfg->count].value, val_start, sizeof(cfg->entries[cfg->count].value) - 1);
        cfg->count++;
    }
    fclose(fp);
    return 0;
}

const char* config_get(const Config *cfg, const char *key) {
    if (!cfg || !key) return NULL;
    for (int i = 0; i < cfg->count; i++) {
        if (strcmp(cfg->entries[i].key, key) == 0) {
            return cfg->entries[i].value;
        }
    }
    return NULL;
}

int config_get_int(const Config *cfg, const char *key, int default_val) {
    const char *val = config_get(cfg, key);
    return val ? atoi(val) : default_val;
}