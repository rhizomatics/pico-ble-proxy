#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void config_defaults(struct config *c) {
    memset(c, 0, sizeof(*c));
    snprintf(c->serial, sizeof(c->serial), "/dev/ttyACM*");
    snprintf(c->listen, sizeof(c->listen), ":6053");
    snprintf(c->name, sizeof(c->name), "boat-ble-proxy");
    snprintf(c->friendly_name, sizeof(c->friendly_name), "Boat BLE Proxy");
    c->flush_ms = 1000;
    c->min_interval_ms = 10000;
    c->keepalive_ms = 120000;
    snprintf(c->maint_listen, sizeof(c->maint_listen), ":4000");
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) {
        s++;
    }
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) {
        *--e = 0;
    }
    return s;
}

// "500ms", "10s", "2m", "1h"; a bare number is seconds.
static int parse_duration(const char *s, int64_t *ms) {
    char *end;
    double v = strtod(s, &end);
    if (end == s || v < 0) {
        return -1;
    }
    double mult;
    if (!*end || !strcmp(end, "s")) {
        mult = 1000;
    } else if (!strcmp(end, "ms")) {
        mult = 1;
    } else if (!strcmp(end, "m")) {
        mult = 60000;
    } else if (!strcmp(end, "h")) {
        mult = 3600000;
    } else {
        return -1;
    }
    *ms = (int64_t)(v * mult);
    return 0;
}

static int parse_bool(const char *s, int *out) {
    if (!strcmp(s, "yes") || !strcmp(s, "true") || !strcmp(s, "1") || !strcmp(s, "on")) {
        *out = 1;
    } else if (!strcmp(s, "no") || !strcmp(s, "false") || !strcmp(s, "0") || !strcmp(s, "off")) {
        *out = 0;
    } else {
        return -1;
    }
    return 0;
}

static int parse_prefixes(char *v, struct prefix *ps, int *n) {
    for (char *tok = strtok(v, " \t,"); tok; tok = strtok(NULL, " \t,")) {
        if (*n >= MAX_PREFIXES || parse_prefix(tok, &ps[*n]) < 0) {
            return -1;
        }
        (*n)++;
    }
    return 0;
}

static int set_str(char *dst, size_t cap, const char *v) {
    if (strlen(v) >= cap) {
        return -1;
    }
    memcpy(dst, v, strlen(v) + 1);
    return 0;
}

int config_parse_line(struct config *c, char *line, const char **err) {
    char *hash = strchr(line, '#');
    if (hash) {
        *hash = 0;
    }
    char *s = trim(line);
    if (!*s) {
        return 0;
    }
    char *key = s;
    while (*s && !isspace((unsigned char)*s)) {
        s++;
    }
    if (*s) {
        *s++ = 0;
    }
    char *val = trim(s);
    int bad = 0;
    *err = "bad value";

    if (!strcmp(key, "serial")) {
        bad = set_str(c->serial, sizeof(c->serial), val);
    } else if (!strcmp(key, "listen")) {
        bad = set_str(c->listen, sizeof(c->listen), val);
    } else if (!strcmp(key, "allow_from")) {
        bad = parse_prefixes(val, c->allow, &c->n_allow);
    } else if (!strcmp(key, "name")) {
        bad = set_str(c->name, sizeof(c->name), val) || !*val;
    } else if (!strcmp(key, "friendly_name")) {
        bad = set_str(c->friendly_name, sizeof(c->friendly_name), val);
    } else if (!strcmp(key, "flush_interval")) {
        bad = parse_duration(val, &c->flush_ms) || c->flush_ms < 10;
    } else if (!strcmp(key, "min_interval")) {
        bad = parse_duration(val, &c->min_interval_ms);
    } else if (!strcmp(key, "keepalive")) {
        bad = parse_duration(val, &c->keepalive_ms);
    } else if (!strcmp(key, "forward_all")) {
        bad = parse_bool(val, &c->forward_all);
    } else if (!strcmp(key, "device")) {
        char *name = val;
        while (*name && !isspace((unsigned char)*name)) {
            name++;
        }
        if (*name) {
            *name++ = 0;
        }
        name = trim(name);
        if (c->n_devices >= MAX_DEVICES) {
            *err = "too many devices";
            return -1;
        }
        struct device *d = &c->devices[c->n_devices];
        bad = parse_mac(val, &d->addr) || set_str(d->name, sizeof(d->name), name);
        if (!bad) {
            c->n_devices++;
        }
    } else if (!strcmp(key, "maintenance_listen")) {
        bad = set_str(c->maint_listen, sizeof(c->maint_listen), val);
    } else if (!strcmp(key, "maintenance_allow_from")) {
        bad = parse_prefixes(val, c->maint_allow, &c->n_maint_allow);
    } else {
        *err = "unknown setting";
        return -1;
    }
    return bad ? -1 : 0;
}

int config_load(const char *path, struct config *c) {
    config_defaults(c);
    FILE *f = fopen(path, "r");
    if (!f) {
        logmsg("%s: %s", path, strerror(errno));
        return -1;
    }
    char line[512];
    int lineno = 0, rc = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char copy[512];
        snprintf(copy, sizeof(copy), "%s", line);
        const char *err;
        if (config_parse_line(c, line, &err) < 0) {
            logmsg("%s:%d: %s: %s", path, lineno, err, trim(copy));
            rc = -1;
        }
    }
    fclose(f);
    return rc;
}

const char *config_device_name(const struct config *c, uint64_t addr) {
    for (int i = 0; i < c->n_devices; i++) {
        if (c->devices[i].addr == addr) {
            return c->devices[i].name;
        }
    }
    return NULL;
}
