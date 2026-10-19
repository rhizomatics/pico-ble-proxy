#include "esphome.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include "util.h"

enum {
    MSG_HELLO_REQUEST = 1,
    MSG_HELLO_RESPONSE = 2,
    MSG_AUTHENTICATION_REQUEST = 3,
    MSG_AUTHENTICATION_RESPONSE = 4,
    MSG_DISCONNECT_REQUEST = 5,
    MSG_DISCONNECT_RESPONSE = 6,
    MSG_PING_REQUEST = 7,
    MSG_PING_RESPONSE = 8,
    MSG_DEVICE_INFO_REQUEST = 9,
    MSG_DEVICE_INFO_RESPONSE = 10,
    MSG_LIST_ENTITIES_REQUEST = 11,
    MSG_LIST_ENTITIES_DONE_RESPONSE = 19,
    MSG_SUBSCRIBE_BLE_ADVERTISEMENTS = 66,
    MSG_UNSUBSCRIBE_BLE_ADVERTISEMENTS = 87,
    MSG_BLE_RAW_ADVERTISEMENTS = 93,
    MSG_BLUETOOTH_SCANNER_STATE = 126,
    MSG_BLUETOOTH_SCANNER_SET_MODE = 127,
    MSG_DEVICE_CAPABILITIES_REQUEST = 149,
    MSG_DEVICE_CAPABILITIES_RESPONSE = 150,
};

// Bluetooth proxy feature flags (bluetooth_proxy.h).
#define FEATURE_PASSIVE_SCAN (1u << 0)
#define FEATURE_RAW_ADVERTISEMENTS (1u << 5)
#define FEATURE_STATE_AND_MODE (1u << 6)
#define FEATURE_FLAGS (FEATURE_PASSIVE_SCAN | FEATURE_RAW_ADVERTISEMENTS | FEATURE_STATE_AND_MODE)

#define SCANNER_STATE_RUNNING 2
#define SCANNER_STATE_STOPPED 5
#define SCANNER_MODE_ACTIVE 1

// API 1.14 is the last version where Home Assistant reads the proxy feature
// flags from DeviceInfoResponse instead of asking for DeviceCapabilities,
// which keeps the handshake small. DeviceCapabilitiesRequest is answered
// anyway in case a client sends it.
#define API_VERSION_MAJOR 1
#define API_VERSION_MINOR 14

// Reported as the ESPHome version. HA raises a repair issue for Bluetooth
// proxies on old ESPHome releases, so this must look current; it says nothing
// about the wire protocol.
#define ESPHOME_VERSION_COMPAT "2026.9.0"

#define BATCH_SIZE 16
#define KEEPALIVE_MS 60000
#define KEEPALIVE_DISCONNECT_MS (KEEPALIVE_MS * 5 / 2)

// --- protobuf encoding -----------------------------------------------------

struct pb {
    uint8_t *p;
    size_t len, cap;
    int overflow;
};

static void pb_raw_varint(struct pb *b, uint64_t v) {
    do {
        uint8_t byte = v & 0x7F;
        v >>= 7;
        if (v) {
            byte |= 0x80;
        }
        if (b->len < b->cap) {
            b->p[b->len++] = byte;
        } else {
            b->overflow = 1;
        }
    } while (v);
}

// Zero values are omitted, as proto3 does.
static void pb_uint(struct pb *b, int field, uint64_t v) {
    if (v == 0) {
        return;
    }
    pb_raw_varint(b, (uint64_t)field << 3 | 0);
    pb_raw_varint(b, v);
}

static void pb_bytes(struct pb *b, int field, const void *data, size_t n) {
    if (n == 0) {
        return;
    }
    pb_raw_varint(b, (uint64_t)field << 3 | 2);
    pb_raw_varint(b, n);
    if (b->len + n > b->cap) {
        b->overflow = 1;
        return;
    }
    memcpy(b->p + b->len, data, n);
    b->len += n;
}

static void pb_str(struct pb *b, int field, const char *s) {
    pb_bytes(b, field, s, strlen(s));
}

static uint64_t zigzag32(int32_t v) {
    return (uint32_t)((uint32_t)v << 1) ^ (uint32_t)(v >> 31);
}

// Returns 1 and advances *pos on success, 0 if more bytes are needed, -1 if malformed.
static int get_varint(const uint8_t *buf, size_t len, size_t *pos, uint64_t *out) {
    uint64_t v = 0;
    for (int shift = 0, i = 0; i < 10; i++, shift += 7) {
        if (*pos + (size_t)i >= len) {
            return 0;
        }
        uint8_t b = buf[*pos + (size_t)i];
        v |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) {
            *pos += (size_t)i + 1;
            *out = v;
            return 1;
        }
    }
    return -1;
}

size_t api_encode_raw_adverts(const struct advert **a, int n, uint8_t *out, size_t cap) {
    struct pb b = {out, 0, cap, 0};
    for (int i = 0; i < n; i++) {
        uint8_t sub_buf[64];
        struct pb sub = {sub_buf, 0, sizeof(sub_buf), 0};
        pb_uint(&sub, 1, a[i]->addr);
        pb_uint(&sub, 2, zigzag32(a[i]->rssi));
        pb_uint(&sub, 3, a[i]->addr_type);
        pb_bytes(&sub, 4, a[i]->data, a[i]->len);
        pb_bytes(&b, 1, sub.p, sub.len);
    }
    return b.overflow ? 0 : b.len;
}

// --- connection handling ---------------------------------------------------

static void client_close(struct api_client *c, const char *why) {
    if (c->fd < 0) {
        return;
    }
    logmsg("api: %s (%s) %s", c->peer, c->client_info[0] ? c->client_info : "?", why);
    close(c->fd);
    c->fd = -1;
}

static void client_flush(struct api_client *c) {
    while (c->out_len > 0) {
        ssize_t n = send(c->fd, c->out, c->out_len, 0);
        if (n > 0) {
            memmove(c->out, c->out + n, c->out_len - (size_t)n);
            c->out_len -= (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            return;
        }
        client_close(c, "write failed");
        return;
    }
    if (c->close_after_flush) {
        client_close(c, "disconnected");
    }
}

static void client_send(struct api_client *c, uint32_t type, const uint8_t *payload, size_t len) {
    if (c->fd < 0) {
        return;
    }
    uint8_t hdr[16];
    struct pb h = {hdr, 0, sizeof(hdr), 0};
    hdr[h.len++] = 0x00;
    pb_raw_varint(&h, len);
    pb_raw_varint(&h, type);
    if (c->out_len + h.len + len > sizeof(c->out)) {
        if (!c->drop_logged) {
            logmsg("api: %s: send queue full, dropping messages", c->peer);
            c->drop_logged = 1;
        }
        return;
    }
    c->drop_logged = 0;
    memcpy(c->out + c->out_len, hdr, h.len);
    memcpy(c->out + c->out_len + h.len, payload, len);
    c->out_len += h.len + len;
    client_flush(c);
}

static void send_adverts(struct api_client *c, const struct advert **a, int n) {
    uint8_t buf[BATCH_SIZE * 64];
    while (n > 0) {
        int k = n < BATCH_SIZE ? n : BATCH_SIZE;
        size_t len = api_encode_raw_adverts(a, k, buf, sizeof(buf));
        if (len) {
            client_send(c, MSG_BLE_RAW_ADVERTISEMENTS, buf, len);
        }
        a += k;
        n -= k;
    }
}

static void send_scanner_state(struct api_server *s, struct api_client *c) {
    uint8_t buf[16];
    struct pb b = {buf, 0, sizeof(buf), 0};
    pb_uint(&b, 1, *s->scanner_running ? SCANNER_STATE_RUNNING : SCANNER_STATE_STOPPED);
    pb_uint(&b, 2, SCANNER_MODE_ACTIVE);  // the firmware always scans actively
    pb_uint(&b, 3, SCANNER_MODE_ACTIVE);
    client_send(c, MSG_BLUETOOTH_SCANNER_STATE, buf, b.len);
}

static void send_device_info(struct api_server *s, struct api_client *c) {
    char mac[18], bt_mac[18], compiled[80];
    fmt_mac_bytes(s->esp->base_mac, mac);
    fmt_mac_bytes(s->esp->bt_mac, bt_mac);
    snprintf(compiled, sizeof(compiled), "bleproxyd %s, fw %s", VERSION, s->esp->firmware);
    uint8_t buf[512];
    struct pb b = {buf, 0, sizeof(buf), 0};
    pb_str(&b, 2, s->cfg->name);
    pb_str(&b, 3, mac);  // HA uses this as the device's unique id
    pb_str(&b, 4, ESPHOME_VERSION_COMPAT);
    pb_str(&b, 5, compiled);
    pb_str(&b, 6, "XIAO ESP32C6 via host");
    pb_str(&b, 12, "Espressif");
    pb_str(&b, 13, s->cfg->friendly_name);
    pb_uint(&b, 15, FEATURE_FLAGS);
    pb_str(&b, 18, bt_mac);
    client_send(c, MSG_DEVICE_INFO_RESPONSE, buf, b.len);
}

static void send_device_capabilities(struct api_server *s, struct api_client *c) {
    char bt_mac[18];
    fmt_mac_bytes(s->esp->bt_mac, bt_mac);
    uint8_t sub_buf[64], buf[80];
    struct pb sub = {sub_buf, 0, sizeof(sub_buf), 0};
    pb_uint(&sub, 1, FEATURE_FLAGS);
    pb_str(&sub, 2, bt_mac);
    struct pb b = {buf, 0, sizeof(buf), 0};
    pb_bytes(&b, 1, sub.p, sub.len);
    client_send(c, MSG_DEVICE_CAPABILITIES_RESPONSE, buf, b.len);
}

static void handle_hello(struct api_server *s, struct api_client *c, const uint8_t *p, size_t n) {
    uint64_t major = 0, minor = 0;
    size_t pos = 0;
    while (pos < n) {
        uint64_t key, v;
        if (get_varint(p, n, &pos, &key) != 1) {
            break;
        }
        int field = (int)(key >> 3), wt = (int)(key & 7);
        if (wt == 0) {
            if (get_varint(p, n, &pos, &v) != 1) {
                break;
            }
            if (field == 2) {
                major = v;
            } else if (field == 3) {
                minor = v;
            }
        } else if (wt == 2) {
            if (get_varint(p, n, &pos, &v) != 1 || v > n - pos) {
                break;
            }
            if (field == 1) {
                size_t l = v < sizeof(c->client_info) - 1 ? v : sizeof(c->client_info) - 1;
                memcpy(c->client_info, p + pos, l);
                c->client_info[l] = 0;
            }
            pos += v;
        } else {
            break;  // HelloRequest has no other wire types
        }
    }
    logmsg("api: %s connected: \"%s\" API %u.%u", c->peer, c->client_info, (unsigned)major, (unsigned)minor);
    uint8_t buf[128];
    struct pb b = {buf, 0, sizeof(buf), 0};
    pb_uint(&b, 1, API_VERSION_MAJOR);
    pb_uint(&b, 2, API_VERSION_MINOR);
    pb_str(&b, 3, "bleproxyd " VERSION);
    pb_str(&b, 4, s->cfg->name);
    client_send(c, MSG_HELLO_RESPONSE, buf, b.len);
}

static void handle_message(struct api_server *s, struct api_client *c, uint64_t type, const uint8_t *p, size_t n) {
    switch (type) {
    case MSG_HELLO_REQUEST:
        handle_hello(s, c, p, n);
        break;
    case MSG_AUTHENTICATION_REQUEST:
        client_send(c, MSG_AUTHENTICATION_RESPONSE, NULL, 0);  // invalid_password = false
        break;
    case MSG_DISCONNECT_REQUEST:
        client_send(c, MSG_DISCONNECT_RESPONSE, NULL, 0);
        c->close_after_flush = 1;
        client_flush(c);
        break;
    case MSG_PING_REQUEST:
        client_send(c, MSG_PING_RESPONSE, NULL, 0);
        break;
    case MSG_DEVICE_INFO_REQUEST:
        send_device_info(s, c);
        break;
    case MSG_DEVICE_CAPABILITIES_REQUEST:
        send_device_capabilities(s, c);
        break;
    case MSG_LIST_ENTITIES_REQUEST:
        client_send(c, MSG_LIST_ENTITIES_DONE_RESPONSE, NULL, 0);
        break;
    case MSG_SUBSCRIBE_BLE_ADVERTISEMENTS: {
        c->subscribed = 1;
        logmsg("api: %s subscribed to advertisements", c->peer);
        send_scanner_state(s, c);
        const struct advert *snap[HUB_MAX];
        send_adverts(c, snap, hub_snapshot(s->hub, snap, HUB_MAX));
        break;
    }
    case MSG_UNSUBSCRIBE_BLE_ADVERTISEMENTS:
        c->subscribed = 0;
        break;
    case MSG_BLUETOOTH_SCANNER_SET_MODE:
        send_scanner_state(s, c);  // mode is fixed in firmware; report it back unchanged
        break;
    default:
        // Subscribe-states, home-assistant-states, logs, ping responses...:
        // nothing to do, and ESPHome devices ignore unknown messages too.
        break;
    }
}

static void client_read(struct api_server *s, struct api_client *c, int64_t now) {
    ssize_t n = recv(c->fd, c->in + c->in_len, sizeof(c->in) - c->in_len, 0);
    if (n == 0) {
        client_close(c, "disconnected");
        return;
    }
    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            client_close(c, strerror(errno));
        }
        return;
    }
    c->in_len += (size_t)n;
    c->last_rx = now;
    c->sent_ping = 0;

    size_t off = 0;
    while (off < c->in_len && c->fd >= 0) {
        if (c->in[off] != 0x00) {
            client_close(c, c->in[off] == 0x01 ? "sent a Noise handshake; remove the encryption key in Home Assistant"
                                               : "sent a bad preamble");
            return;
        }
        size_t pos = off + 1;
        uint64_t len, type;
        int r = get_varint(c->in, c->in_len, &pos, &len);
        if (r == 1) {
            r = get_varint(c->in, c->in_len, &pos, &type);
        }
        if (r < 0 || (r == 1 && len > sizeof(c->in) - 32)) {
            client_close(c, "sent a malformed message");
            return;
        }
        if (r == 0 || c->in_len - pos < len) {
            break;  // wait for the rest
        }
        handle_message(s, c, type, c->in + pos, (size_t)len);
        off = pos + (size_t)len;
    }
    if (c->fd >= 0) {
        memmove(c->in, c->in + off, c->in_len - off);
        c->in_len -= off;
    }
}

static void accept_client(struct api_server *s, int64_t now) {
    struct sockaddr_in sa;
    socklen_t sl = sizeof(sa);
    int fd = accept(s->lfd, (struct sockaddr *)&sa, &sl);
    if (fd < 0) {
        return;
    }
    char peer[24];
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &sa.sin_addr, ip, sizeof(ip));
    snprintf(peer, sizeof(peer), "%s:%u", ip, ntohs(sa.sin_port));
    if (s->cfg->n_allow > 0 && !prefix_match(s->cfg->allow, s->cfg->n_allow, ntohl(sa.sin_addr.s_addr))) {
        logmsg("api: rejected connection from %s (not in allow_from)", peer);
        close(fd);
        return;
    }
    for (int i = 0; i < API_MAX_CLIENTS; i++) {
        struct api_client *c = &s->cl[i];
        if (c->fd >= 0) {
            continue;
        }
        memset(c, 0, sizeof(*c));
        c->fd = fd;
        c->last_rx = now;
        snprintf(c->peer, sizeof(c->peer), "%s", peer);
        set_nonblock(fd);
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        return;
    }
    logmsg("api: rejected connection from %s (too many clients)", peer);
    close(fd);
}

// --- public ----------------------------------------------------------------

void api_init(struct api_server *s, const struct config *cfg, const struct esp_info *esp, struct hub *hub,
              const int *scanner_running) {
    memset(s, 0, sizeof(*s));
    s->lfd = -1;
    s->cfg = cfg;
    s->esp = esp;
    s->hub = hub;
    s->scanner_running = scanner_running;
    for (int i = 0; i < API_MAX_CLIENTS; i++) {
        s->cl[i].fd = -1;
    }
}

int api_listen(struct api_server *s) {
    s->lfd = listen_tcp(s->cfg->listen);
    return s->lfd < 0 ? -1 : 0;
}

int api_pollfds(struct api_server *s, struct pollfd *p) {
    int n = 0;
    if (s->lfd < 0) {
        return 0;
    }
    p[n++] = (struct pollfd){.fd = s->lfd, .events = POLLIN};
    for (int i = 0; i < API_MAX_CLIENTS; i++) {
        struct api_client *c = &s->cl[i];
        // Unused slots get fd -1, which poll() ignores, so indices stay fixed.
        p[n++] = (struct pollfd){.fd = c->fd, .events = (short)(POLLIN | (c->out_len ? POLLOUT : 0))};
    }
    return n;
}

void api_process(struct api_server *s, const struct pollfd *p, int n, int64_t now) {
    if (n == 0) {
        return;
    }
    for (int i = 0; i < API_MAX_CLIENTS && i + 1 < n; i++) {
        struct api_client *c = &s->cl[i];
        short re = p[i + 1].revents;
        if (c->fd < 0 || c->fd != p[i + 1].fd || !re) {
            continue;
        }
        if (re & (POLLIN | POLLHUP | POLLERR)) {
            client_read(s, c, now);
        }
        if (c->fd >= 0 && (re & POLLOUT)) {
            client_flush(c);
        }
    }
    if (p[0].revents & POLLIN) {
        accept_client(s, now);
    }
}

void api_tick(struct api_server *s, int64_t now) {
    for (int i = 0; i < API_MAX_CLIENTS; i++) {
        struct api_client *c = &s->cl[i];
        if (c->fd < 0) {
            continue;
        }
        int64_t idle = now - c->last_rx;
        if (idle > KEEPALIVE_DISCONNECT_MS) {
            client_close(c, "timed out");
        } else if (idle > KEEPALIVE_MS && !c->sent_ping) {
            // A client receiving a steady stream doesn't ping, so check on it
            // ourselves, as ESPHome devices do.
            client_send(c, MSG_PING_REQUEST, NULL, 0);
            c->sent_ping = 1;
        }
    }
}

int api_subscribers(const struct api_server *s) {
    int n = 0;
    for (int i = 0; i < API_MAX_CLIENTS; i++) {
        n += s->cl[i].fd >= 0 && s->cl[i].subscribed;
    }
    return n;
}

void api_broadcast_adverts(struct api_server *s, const struct advert **a, int n) {
    for (int i = 0; i < API_MAX_CLIENTS; i++) {
        if (s->cl[i].fd >= 0 && s->cl[i].subscribed) {
            send_adverts(&s->cl[i], a, n);
        }
    }
}

void api_broadcast_scanner_state(struct api_server *s) {
    for (int i = 0; i < API_MAX_CLIENTS; i++) {
        if (s->cl[i].fd >= 0 && s->cl[i].subscribed) {
            send_scanner_state(s, &s->cl[i]);
        }
    }
}
