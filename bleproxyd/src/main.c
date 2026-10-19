// bleproxyd runs on the host. It reads BLE advertisements from an ESP32 on
// USB and presents them to Home Assistant as an ESPHome Bluetooth proxy.

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "config.h"
#include "esphome.h"
#include "hub.h"
#include "linkcodec.h"
#include "rfc2217.h"
#include "serial.h"
#include "util.h"

static volatile sig_atomic_t stop;

static void on_signal(int sig) {
    (void)sig;
    stop = 1;
}

static void usage(void) {
    fprintf(stderr,
            "bleproxyd %s\n\n"
            "usage:\n"
            "  bleproxyd run          [-c FILE]            serve Home Assistant (default)\n"
            "  bleproxyd discover     [-c FILE] [-t SECS]  list nearby BLE devices\n"
            "  bleproxyd flash-bridge [-c FILE] [-t SECS]  RFC 2217 bridge for esptool\n"
            "  bleproxyd reboot-esp   [-c FILE]\n"
            "  bleproxyd check-config [-c FILE]\n"
            "  bleproxyd version\n\n"
            "Default config: /etc/bleproxyd.conf. Stop the service before discover,\n"
            "flash-bridge or reboot-esp, which need the serial port to themselves.\n",
            VERSION);
}

// --- serial connection shared by run and discover -------------------------

struct esp_link {
    const char *pattern;
    int fd;
    int64_t next_open;
    char last_err[128];
    struct link_reader reader;
};

static void link_close(struct esp_link *l, int64_t now) {
    if (l->fd >= 0) {
        close(l->fd);
        l->fd = -1;
    }
    l->next_open = now + 1000;
}

// Opens the port when due. Returns 1 if it was just opened.
static int link_maybe_open(struct esp_link *l, int64_t now) {
    if (l->fd >= 0 || now < l->next_open) {
        return 0;
    }
    char name[128];
    l->fd = serial_open(l->pattern, name, sizeof(name));
    if (l->fd < 0) {
        char err[128];
        snprintf(err, sizeof(err), "%s: %s", l->pattern, strerror(errno));
        if (strcmp(err, l->last_err)) {  // log changes only
            logmsg("serial: %s (retrying)", err);
            snprintf(l->last_err, sizeof(l->last_err), "%s", err);
        }
        l->next_open = now + 2000;
        return 0;
    }
    l->last_err[0] = 0;
    logmsg("serial: opened %s", name);
    link_reader_init(&l->reader);
    uint8_t f[LINK_ENC_MAX];
    size_t n = link_frame_encode(LINK_CMD_HELLO_REQ, NULL, 0, f);
    write_all(l->fd, f, n, 1000);
    return 1;
}

typedef void (*frame_fn)(void *ctx, uint8_t type, const uint8_t *p, size_t n);

// Reads what's available. Returns -1 if the connection dropped.
static int link_read(struct esp_link *l, frame_fn fn, void *ctx) {
    uint8_t buf[4096], payload[LINK_MAX_PAYLOAD], type;
    ssize_t n = read(l->fd, buf, sizeof(buf));
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        return 0;
    }
    if (n <= 0) {
        logmsg("serial: %s", n == 0 ? "closed" : strerror(errno));
        return -1;
    }
    for (ssize_t i = 0; i < n; i++) {
        size_t len;
        if (link_reader_feed(&l->reader, buf[i], &type, payload, &len)) {
            fn(ctx, type, payload, len);
        }
    }
    return 0;
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

// --- run ---------------------------------------------------------------------

struct run_ctx {
    struct config *cfg;
    struct hub hub;
    struct esp_info esp;
    int have_esp;
    int scanner_running;
    struct api_server api;
};

static void set_scanner(struct run_ctx *r, int running) {
    if (r->scanner_running != running) {
        r->scanner_running = running;
        api_broadcast_scanner_state(&r->api);
    }
}

static void run_frame(void *ctx, uint8_t type, const uint8_t *p, size_t n) {
    struct run_ctx *r = ctx;
    switch (type) {
        case LINK_ADV: {
            struct advert a;
            if (advert_parse(p, n, &a) == 0) {
                hub_ingest(&r->hub, &a, now_ms());
            }
            break;
        }
        case LINK_HELLO:
            if (n >= 13) {
                memcpy(r->esp.base_mac, p + 1, 6);
                memcpy(r->esp.bt_mac, p + 7, 6);
                size_t fl = n - 13 < sizeof(r->esp.firmware) - 1 ? n - 13 : sizeof(r->esp.firmware) - 1;
                memcpy(r->esp.firmware, p + 13, fl);
                r->esp.firmware[fl] = 0;
                char bt[18];
                fmt_mac_bytes(r->esp.bt_mac, bt);
                logmsg("esp: firmware \"%s\" bt %s", r->esp.firmware, bt);
                r->have_esp = 1;
                set_scanner(r, 1);
            }
            break;
        case LINK_STATUS:
            // Every ~10 minutes, so the log shows the link is healthy.
            if (n >= 16 && le32(p) % 600 < 30) {
                logmsg("esp: up %us seen %u dropped %u heap %u | hub seen %lu allowed %lu forwarded %lu", le32(p),
                       le32(p + 4), le32(p + 8), le32(p + 12), r->hub.seen, r->hub.allowed, r->hub.forwarded);
            }
            break;
        case LINK_LOG:
            logmsg("esp: %.*s", (int)n, (const char *)p);
            break;
    }
}

static int cmd_run(struct config *cfg) {
    static struct run_ctx r;  // large: client buffers
    memset(&r, 0, sizeof(r));
    r.cfg = cfg;
    hub_init(&r.hub, cfg);
    api_init(&r.api, cfg, &r.esp, &r.hub, &r.scanner_running);

    if (cfg->forward_all) {
        logmsg("forwarding all devices (forward_all)");
    }
    for (int i = 0; i < cfg->n_devices; i++) {
        char mac[18];
        fmt_mac(cfg->devices[i].addr, mac);
        logmsg("forwarding %s %s", mac, cfg->devices[i].name);
    }
    if (!cfg->forward_all && cfg->n_devices == 0) {
        logmsg("warning: no devices configured and forward_all is off; nothing will be forwarded");
    }
    logmsg("waiting for ESP32 on %s", cfg->serial);

    struct esp_link l = {.pattern = cfg->serial, .fd = -1};
    int64_t next_flush = now_ms() + cfg->flush_ms;
    while (!stop) {
        int64_t now = now_ms();
        link_maybe_open(&l, now);

        // Don't accept API connections until we know the ESP's MAC
        // addresses, because Home Assistant uses them as the device identity.
        if (r.have_esp && r.api.lfd < 0) {
            if (api_listen(&r.api) < 0) {
                logmsg("api: can't listen on %s: %s", cfg->listen, strerror(errno));
                return 1;
            }
            logmsg("ESPHome API listening on %s", cfg->listen);
        }

        struct pollfd p[1 + 1 + API_MAX_CLIENTS];
        int np = 0;
        p[np++] = (struct pollfd){.fd = l.fd, .events = POLLIN};
        int api_n = api_pollfds(&r.api, p + np);
        np += api_n;

        int timeout = (int)(next_flush - now);
        if (timeout < 0) {
            timeout = 0;
        }
        if (timeout > 500) {
            timeout = 500;
        }
        if (poll(p, (nfds_t)np, timeout) < 0 && errno != EINTR) {
            logmsg("poll: %s", strerror(errno));
            return 1;
        }
        now = now_ms();
        if (l.fd >= 0 && p[0].revents) {
            if (link_read(&l, run_frame, &r) < 0) {
                link_close(&l, now);
                set_scanner(&r, 0);
            }
        }
        api_process(&r.api, p + 1, api_n, now);
        if (now >= next_flush) {
            next_flush = now + cfg->flush_ms;
            // Leave adverts pending while nobody listens; a new subscriber
            // gets a snapshot anyway.
            if (api_subscribers(&r.api) > 0) {
                const struct advert *due[64];
                int n;
                while ((n = hub_take_pending(&r.hub, now, due, 64)) > 0) {
                    api_broadcast_adverts(&r.api, due, n);
                }
            }
        }
        api_tick(&r.api, now);
    }
    logmsg("stopping");
    return 0;
}

// --- discover ----------------------------------------------------------------

#define DISCOVER_MAX 512

struct seen_device {
    uint64_t addr;
    uint8_t addr_type;
    int8_t rssi;
    unsigned count;
    char name[32];
    char hints[64];
};

struct discover_ctx {
    struct seen_device d[DISCOVER_MAX];
    int n;
};

static const struct {
    uint16_t id;
    const char *name;
} company_names[] =
    {
        {0x02E1, "Victron"},   {0x0499, "Ruuvi"},   {0x0969, "SwitchBot"}, {0x004C, "Apple"},
        {0x0006, "Microsoft"}, {0x0075, "Samsung"}, {0x00E0, "Google"},
    },
  service_names[] = {
      {0xFD3D, "SwitchBot"},
      {0xFCD2, "BTHome"},
      {0xFE95, "Xiaomi"},
      {0xFEAA, "Eddystone"},
  };

static void add_hint(struct seen_device *s, const char *h) {
    if (strstr(s->hints, h)) {
        return;
    }
    size_t l = strlen(s->hints);
    snprintf(s->hints + l, sizeof(s->hints) - l, "%s%s", l ? "," : "", h);
}

static void describe(struct seen_device *s, const uint8_t *d, size_t n) {
    while (n >= 2) {
        size_t l = d[0];
        if (l == 0 || l + 1 > n) {
            return;
        }
        uint8_t type = d[1];
        const uint8_t *v = d + 2;
        size_t vl = l - 1;
        char hint[16];
        if ((type == 0x08 || type == 0x09) && vl) {  // local name
            snprintf(s->name, sizeof(s->name), "%.*s", (int)vl, (const char *)v);
        } else if ((type == 0xFF || type == 0x16) && vl >= 2) {  // manufacturer / service data
            uint16_t id = (uint16_t)(v[0] | v[1] << 8);
            const char *known = NULL;
            if (type == 0xFF) {
                for (size_t i = 0; i < sizeof(company_names) / sizeof(company_names[0]); i++) {
                    if (company_names[i].id == id) {
                        known = company_names[i].name;
                    }
                }
            } else {
                for (size_t i = 0; i < sizeof(service_names) / sizeof(service_names[0]); i++) {
                    if (service_names[i].id == id) {
                        known = service_names[i].name;
                    }
                }
            }
            if (!known) {
                snprintf(hint, sizeof(hint), "%s:%04X", type == 0xFF ? "mfr" : "svc", id);
                known = hint;
            }
            add_hint(s, known);
        }
        d += l + 1;
        n -= l + 1;
    }
}

static void discover_frame(void *ctx, uint8_t type, const uint8_t *p, size_t n) {
    struct discover_ctx *dc = ctx;
    struct advert a;
    if (type != LINK_ADV || advert_parse(p, n, &a) < 0) {
        return;
    }
    struct seen_device *s = NULL;
    for (int i = 0; i < dc->n; i++) {
        if (dc->d[i].addr == a.addr) {
            s = &dc->d[i];
        }
    }
    if (!s) {
        if (dc->n == DISCOVER_MAX) {
            return;
        }
        s = &dc->d[dc->n++];
        memset(s, 0, sizeof(*s));
        s->addr = a.addr;
        s->addr_type = a.addr_type;
    }
    s->count++;
    s->rssi = a.rssi;
    describe(s, a.data, a.len);
}

static int by_rssi(const void *a, const void *b) {
    return ((const struct seen_device *)b)->rssi - ((const struct seen_device *)a)->rssi;
}

static int cmd_discover(struct config *cfg, int secs) {
    static struct discover_ctx dc;
    struct esp_link l = {.pattern = cfg->serial, .fd = -1};
    fprintf(stderr, "listening for %ds...\n", secs);
    int64_t end = now_ms() + secs * 1000LL;
    while (!stop && now_ms() < end) {
        int64_t now = now_ms();
        link_maybe_open(&l, now);
        struct pollfd p = {.fd = l.fd, .events = POLLIN};
        poll(&p, 1, 200);
        if (l.fd >= 0 && p.revents && link_read(&l, discover_frame, &dc) < 0) {
            link_close(&l, now_ms());
        }
    }
    qsort(dc.d, (size_t)dc.n, sizeof(dc.d[0]), by_rssi);
    printf("  %-17s  %-4s  %-4s  %-5s  %-20s  %s\n", "MAC", "TYPE", "RSSI", "ADVS", "NAME", "VENDOR HINTS");
    for (int i = 0; i < dc.n; i++) {
        struct seen_device *s = &dc.d[i];
        char mac[18];
        fmt_mac(s->addr, mac);
        const char *cfg_name = config_device_name(cfg, s->addr);
        char name[40];
        snprintf(name, sizeof(name), "%s", s->name[0] ? s->name : "");
        if (!name[0] && cfg_name) {
            snprintf(name, sizeof(name), "(%s)", cfg_name);
        }
        printf("%s %-17s  %-4u  %-4d  %-5u  %-20s  %s\n", cfg_name ? "*" : " ", mac, s->addr_type, s->rssi, s->count,
               name, s->hints);
    }
    fflush(stdout);
    fprintf(stderr, "%d devices; * = in allowlist\n", dc.n);
    return 0;
}

// --- flash-bridge / reboot-esp -------------------------------------------------

static int cmd_flash_bridge(struct config *cfg, int idle_secs) {
    if (cfg->n_maint_allow == 0) {
        logmsg("maintenance_allow_from is empty; refusing to expose the serial port to everyone");
        return 1;
    }
    int lfd = listen_tcp(cfg->maint_listen);
    if (lfd < 0) {
        logmsg("flash-bridge: can't listen on %s: %s", cfg->maint_listen, strerror(errno));
        return 1;
    }
    logmsg("flash-bridge: listening on %s for %ds", cfg->maint_listen, idle_secs);
    int64_t end = now_ms() + idle_secs * 1000LL;
    while (!stop && now_ms() < end) {
        struct pollfd p = {.fd = lfd, .events = POLLIN};
        if (poll(&p, 1, 500) <= 0) {
            continue;
        }
        struct sockaddr_in sa;
        socklen_t sl = sizeof(sa);
        int cfd = accept(lfd, (struct sockaddr *)&sa, &sl);
        if (cfd < 0) {
            continue;
        }
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &sa.sin_addr, ip, sizeof(ip));
        if (!prefix_match(cfg->maint_allow, cfg->n_maint_allow, ntohl(sa.sin_addr.s_addr))) {
            logmsg("flash-bridge: rejected %s", ip);
            close(cfd);
            continue;
        }
        close(lfd);
        logmsg("flash-bridge: client %s", ip);
        int rc = rfc2217_session(cfd, cfg->serial);
        close(cfd);
        logmsg("flash-bridge: session ended%s", rc ? " with an error" : "");
        return rc ? 1 : 0;
    }
    logmsg("flash-bridge: idle timeout");
    close(lfd);
    return 0;
}

static int cmd_reboot_esp(struct config *cfg) {
    char name[128];
    int fd = serial_open(cfg->serial, name, sizeof(name));
    if (fd < 0) {
        logmsg("%s: %s", cfg->serial, strerror(errno));
        return 1;
    }
    uint8_t f[LINK_ENC_MAX];
    size_t n = link_frame_encode(LINK_CMD_REBOOT, NULL, 0, f);
    int rc = write_all(fd, f, n, 2000);
    close(fd);
    return rc < 0;
}

int main(int argc, char **argv) {
    const char *cmd = "run";
    int argi = 1;
    if (argc > 1 && argv[1][0] != '-') {
        cmd = argv[1];
        argi = 2;
    }
    const char *cfg_path = "/etc/bleproxyd.conf";
    int secs = -1;
    for (; argi < argc; argi++) {
        if (!strcmp(argv[argi], "-c") && argi + 1 < argc) {
            cfg_path = argv[++argi];
        } else if (!strcmp(argv[argi], "-t") && argi + 1 < argc) {
            secs = atoi(argv[++argi]);
        } else {
            usage();
            return 2;
        }
    }
    if (!strcmp(cmd, "version")) {
        printf("%s\n", VERSION);
        return 0;
    }
    static struct config cfg;
    if (config_load(cfg_path, &cfg) < 0) {
        return 1;
    }
    if (!strcmp(cmd, "check-config")) {
        printf("%s: ok, %d devices%s\n", cfg_path, cfg.n_devices, cfg.forward_all ? ", forward_all" : "");
        return 0;
    }

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa = {.sa_handler = on_signal};
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    if (!strcmp(cmd, "run")) {
        return cmd_run(&cfg);
    } else if (!strcmp(cmd, "discover")) {
        return cmd_discover(&cfg, secs > 0 ? secs : 30);
    } else if (!strcmp(cmd, "flash-bridge")) {
        return cmd_flash_bridge(&cfg, secs > 0 ? secs : 600);
    } else if (!strcmp(cmd, "reboot-esp")) {
        return cmd_reboot_esp(&cfg);
    }
    usage();
    return 2;
}
