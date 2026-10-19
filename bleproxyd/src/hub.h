// Decides which advertisements are forwarded to Home Assistant, and how often.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "config.h"

#define ADV_DATA_MAX 31
// Distinct (address, packet kind) pairs tracked. When full, the least
// recently heard is evicted.
#define HUB_MAX      256

struct advert {
    uint64_t addr;
    uint8_t addr_type;
    int8_t rssi;
    uint8_t scan_rsp;
    uint8_t len;
    uint8_t data[ADV_DATA_MAX];
};

// Parses a LINK_ADV payload.
int advert_parse(const uint8_t *p, size_t n, struct advert *a);

struct hub_entry {
    struct advert last;
    uint8_t used, pending, ever_sent;
    uint8_t sent_len;
    uint8_t sent_data[ADV_DATA_MAX];
    int64_t last_sent, last_seen;
};

struct hub {
    const struct config *cfg;
    struct hub_entry e[HUB_MAX];
    unsigned long seen, allowed, forwarded;
};

void hub_init(struct hub *h, const struct config *cfg);

// Records an advert and marks it for forwarding when it is due:
//  - never more often than min_interval per device and packet kind, however
//    often the payload changes (Ruuvi/Victron counters change every advert);
//  - an unchanged payload again after keepalive, so Home Assistant doesn't
//    mark passive devices unavailable (it gives up after ~15 min).
void hub_ingest(struct hub *h, const struct advert *a, int64_t now);

// Fills out with up to max adverts due for forwarding and marks them sent.
// The pointers stay valid until the next hub_ingest.
int hub_take_pending(struct hub *h, int64_t now, const struct advert **out, int max);

// Latest advert of every tracked device, for replaying to a new subscriber.
int hub_snapshot(struct hub *h, const struct advert **out, int max);
