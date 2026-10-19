#include "hub.h"

#include <string.h>

int advert_parse(const uint8_t *p, size_t n, struct advert *a) {
    if (n < 9 || n > 9 + ADV_DATA_MAX) {
        return -1;
    }
    a->addr = 0;
    for (int i = 0; i < 6; i++) {
        a->addr = a->addr << 8 | p[i];
    }
    a->addr_type = p[6];
    a->rssi = (int8_t)p[7];
    a->scan_rsp = p[8] == 1;
    a->len = (uint8_t)(n - 9);
    memcpy(a->data, p + 9, a->len);
    return 0;
}

void hub_init(struct hub *h, const struct config *cfg) {
    memset(h, 0, sizeof(*h));
    h->cfg = cfg;
}

static struct hub_entry *lookup(struct hub *h, const struct advert *a) {
    struct hub_entry *free_slot = NULL, *oldest = NULL;
    for (int i = 0; i < HUB_MAX; i++) {
        struct hub_entry *e = &h->e[i];
        if (!e->used) {
            if (!free_slot) {
                free_slot = e;
            }
            continue;
        }
        if (e->last.addr == a->addr && e->last.scan_rsp == a->scan_rsp) {
            return e;
        }
        if (!oldest || e->last_seen < oldest->last_seen) {
            oldest = e;
        }
    }
    struct hub_entry *e = free_slot ? free_slot : oldest;
    memset(e, 0, sizeof(*e));
    e->used = 1;
    return e;
}

void hub_ingest(struct hub *h, const struct advert *a, int64_t now) {
    h->seen++;
    // Active scanning gets empty scan responses from many devices (Apple
    // especially); they carry nothing for Home Assistant.
    if (a->len == 0) {
        return;
    }
    if (!h->cfg->forward_all && !config_device_name(h->cfg, a->addr)) {
        return;
    }
    h->allowed++;
    struct hub_entry *e = lookup(h, a);
    e->last = *a;
    e->last_seen = now;
    if (!e->ever_sent) {
        e->pending = 1;
        return;
    }
    int64_t since = now - e->last_sent;
    int changed = a->len != e->sent_len || memcmp(a->data, e->sent_data, a->len) != 0;
    if (since >= h->cfg->min_interval_ms && (changed || since >= h->cfg->keepalive_ms)) {
        e->pending = 1;
    }
}

int hub_take_pending(struct hub *h, int64_t now, const struct advert **out, int max) {
    int n = 0;
    for (int i = 0; i < HUB_MAX && n < max; i++) {
        struct hub_entry *e = &h->e[i];
        if (!e->used || !e->pending) {
            continue;
        }
        out[n++] = &e->last;
        e->pending = 0;
        e->ever_sent = 1;
        e->last_sent = now;
        e->sent_len = e->last.len;
        memcpy(e->sent_data, e->last.data, e->last.len);
    }
    h->forwarded += (unsigned long)n;
    return n;
}

int hub_snapshot(struct hub *h, const struct advert **out, int max) {
    int n = 0;
    for (int i = 0; i < HUB_MAX && n < max; i++) {
        if (h->e[i].used) {
            out[n++] = &h->e[i].last;
        }
    }
    return n;
}
