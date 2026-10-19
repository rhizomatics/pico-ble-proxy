// The minimum of the ESPHome native API (plaintext transport) for Home
// Assistant to treat bleproxyd as a passive Bluetooth proxy. Message ids and
// field numbers come from esphome/components/api/api.proto.
#pragma once

#include <poll.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "hub.h"

#define API_MAX_CLIENTS 4
#define API_IN_MAX 4096
#define API_OUT_MAX 32768

// What the ESP told us in its HELLO frame.
struct esp_info {
    uint8_t base_mac[6];
    uint8_t bt_mac[6];
    char firmware[33];
};

struct api_client {
    int fd;
    char peer[24];
    char client_info[64];
    uint8_t in[API_IN_MAX];
    size_t in_len;
    uint8_t out[API_OUT_MAX];
    size_t out_len;
    int subscribed;
    int64_t last_rx;
    int sent_ping;
    int close_after_flush;
    int drop_logged;
};

struct api_server {
    int lfd;
    const struct config *cfg;
    const struct esp_info *esp;
    struct hub *hub;
    const int *scanner_running;
    struct api_client cl[API_MAX_CLIENTS];
};

void api_init(struct api_server *s, const struct config *cfg, const struct esp_info *esp, struct hub *hub,
              const int *scanner_running);
int api_listen(struct api_server *s);

// Poll integration: api_pollfds appends the server's fds and returns how many;
// api_process handles the same slice after poll().
int api_pollfds(struct api_server *s, struct pollfd *p);
void api_process(struct api_server *s, const struct pollfd *p, int n, int64_t now);
// Keepalive: ping clients silent for 60 s, drop them at 150 s.
void api_tick(struct api_server *s, int64_t now);

int api_subscribers(const struct api_server *s);
void api_broadcast_adverts(struct api_server *s, const struct advert **a, int n);
void api_broadcast_scanner_state(struct api_server *s);

// Exposed for tests.
size_t api_encode_raw_adverts(const struct advert **a, int n, uint8_t *out, size_t cap);
