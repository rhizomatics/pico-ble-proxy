#pragma once

#include <stdint.h>

#include "util.h"

#define MAX_DEVICES  64
#define MAX_PREFIXES 16

struct device {
    uint64_t addr;
    char name[48];
};

struct config {
    char serial[128];  // device path, glob allowed; tcp://host:port for development

    char listen[64];  // ESPHome API
    struct prefix allow[MAX_PREFIXES];
    int n_allow;

    char name[32];  // ESPHome hostname
    char friendly_name[64];

    int64_t flush_ms;
    int64_t min_interval_ms;
    int64_t keepalive_ms;

    int forward_all;
    struct device devices[MAX_DEVICES];
    int n_devices;

    char maint_listen[64];  // RFC 2217 flash bridge
    struct prefix maint_allow[MAX_PREFIXES];
    int n_maint_allow;
};

void config_defaults(struct config *c);
// Returns 0 on success; logs the offending line otherwise.
int config_load(const char *path, struct config *c);
int config_parse_line(struct config *c, char *line, const char **err);
// Name of the allowlisted device, or NULL if it isn't forwarded.
const char *config_device_name(const struct config *c, uint64_t addr);
