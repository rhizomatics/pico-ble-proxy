// Unit tests for the parts with no I/O: framing, rate limiting, config,
// protobuf encoding. Run with `make test` (built with ASan/UBSan).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "esphome.h"
#include "hub.h"
#include "linkcodec.h"

static int failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static void test_crc(void) {
    CHECK(link_crc16((const uint8_t *)"123456789", 9) == 0x29B1);
}

static void test_cobs_roundtrip(void) {
    size_t lens[] = {0, 1, 2, 253, 254, 255, 300};
    for (size_t li = 0; li < sizeof(lens) / sizeof(lens[0]); li++) {
        for (int pattern = 0; pattern < 3; pattern++) {
            uint8_t in[300], enc[310], dec[300];
            size_t n = lens[li];
            for (size_t i = 0; i < n; i++) {
                in[i] = pattern == 0 ? 0 : pattern == 1 ? 0xAA : (uint8_t)(i % 7);
            }
            size_t e = link_cobs_encode(in, n, enc);
            CHECK(memchr(enc, 0, e) == NULL);
            size_t d = link_cobs_decode(enc, e, dec, sizeof(dec));
            CHECK(d == n || (n == 0 && d == 0));
            CHECK(memcmp(in, dec, n) == 0);
        }
    }
    // Decoder must refuse output that doesn't fit.
    uint8_t in[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10}, enc[16], dec[4];
    size_t e = link_cobs_encode(in, 10, enc);
    CHECK(link_cobs_decode(enc, e, dec, sizeof(dec)) == 0);
}

static void test_reader_skips_noise(void) {
    const uint8_t adv[] = {0xF2, 0xF3, 0xC2, 0x66, 0xFE, 0xA0, 1, 0xB0, 0, 0x02, 0x01, 0x06};
    uint8_t stream[512];
    size_t n = 0;
    const char *boot = "ESP-ROM:esp32c6-20220919\r\nbuild: Sep 19 2022\r\n";
    memcpy(stream, boot, strlen(boot));
    n += strlen(boot);
    n += link_frame_encode(LINK_ADV, adv, sizeof(adv), stream + n);
    size_t c = link_frame_encode(LINK_ADV, adv, sizeof(adv), stream + n);
    stream[n + 3] ^= 0x40;  // corrupt the second copy
    n += c;
    n += link_frame_encode(LINK_LOG, (const uint8_t *)"hi", 2, stream + n);

    struct link_reader r;
    link_reader_init(&r);
    uint8_t type, payload[LINK_MAX_PAYLOAD];
    size_t len;
    int got = 0;
    for (size_t i = 0; i < n; i++) {
        if (link_reader_feed(&r, stream[i], &type, payload, &len)) {
            if (got == 0) {
                CHECK(type == LINK_ADV && len == sizeof(adv) && !memcmp(payload, adv, len));
                struct advert a;
                CHECK(advert_parse(payload, len, &a) == 0);
                CHECK(a.addr == 0xF2F3C266FEA0ULL && a.rssi == -80 && a.addr_type == 1 && !a.scan_rsp);
            } else {
                CHECK(type == LINK_LOG && len == 2 && !memcmp(payload, "hi", 2));
            }
            got++;
        }
    }
    CHECK(got == 2);
    CHECK(r.bad == 2);  // boot text + corrupted frame
}

static struct advert ruuvi(uint8_t seq) {
    struct advert a = {.addr = 0xF2F3C266FEA0ULL, .rssi = -70, .len = 4, .data = {0xFF, 0x99, 0x04, seq}};
    return a;
}

static void test_hub_policy(void) {
    static struct config cfg;
    config_defaults(&cfg);
    cfg.min_interval_ms = 10000;
    cfg.keepalive_ms = 60000;
    cfg.devices[0].addr = 0xF2F3C266FEA0ULL;
    cfg.n_devices = 1;
    static struct hub h;
    hub_init(&h, &cfg);
    const struct advert *out[8];
    int64_t t0 = 1000000;

    struct advert stranger = {.addr = 0x112233445566ULL, .len = 1, .data = {1}};
    hub_ingest(&h, &stranger, t0);
    struct advert empty = {.addr = 0xF2F3C266FEA0ULL, .scan_rsp = 1};
    hub_ingest(&h, &empty, t0);
    struct advert a = ruuvi(1);
    hub_ingest(&h, &a, t0);
    CHECK(hub_take_pending(&h, t0, out, 8) == 1);

    a = ruuvi(2);  // changed, but inside min_interval: held back
    hub_ingest(&h, &a, t0 + 3000);
    CHECK(hub_take_pending(&h, t0 + 3000, out, 8) == 0);
    a = ruuvi(3);  // due: the latest payload goes
    hub_ingest(&h, &a, t0 + 11000);
    CHECK(hub_take_pending(&h, t0 + 11000, out, 8) == 1 && out[0]->data[3] == 3);

    hub_ingest(&h, &a, t0 + 30000);  // unchanged: waits for keepalive
    CHECK(hub_take_pending(&h, t0 + 30000, out, 8) == 0);
    hub_ingest(&h, &a, t0 + 72000);
    CHECK(hub_take_pending(&h, t0 + 72000, out, 8) == 1);

    CHECK(h.seen == 7 && h.allowed == 5 && h.forwarded == 3);
    CHECK(hub_snapshot(&h, out, 8) == 1);
}

static void test_hub_eviction(void) {
    static struct config cfg;
    config_defaults(&cfg);
    cfg.forward_all = 1;
    static struct hub h;
    hub_init(&h, &cfg);
    for (int i = 0; i < HUB_MAX + 10; i++) {
        struct advert a = {.addr = (uint64_t)i + 1, .len = 1, .data = {1}};
        hub_ingest(&h, &a, i);
    }
    const struct advert *out[HUB_MAX];
    CHECK(hub_snapshot(&h, out, HUB_MAX) == HUB_MAX);
    int has_first = 0, has_last = 0;
    for (int i = 0; i < HUB_MAX; i++) {
        has_first |= out[i]->addr == 1;
        has_last |= out[i]->addr == HUB_MAX + 10;
    }
    CHECK(!has_first && has_last);  // oldest evicted
}

static void test_config(void) {
    static struct config c;
    config_defaults(&c);
    const char *err;
    char l1[] = "listen 192.168.1.1:6053   # comment";
    char l2[] = "allow_from 192.168.0.10 192.168.0.0/24";
    char l3[] = "device 00:11:22:33:44:01  Victron solar charger";
    char l4[] = "min_interval 2.5s";
    char l5[] = "keepalive 2m";
    char l6[] = "flush_interval 500ms";
    char l7[] = "   # just a comment";
    char l8[] = "forward_all yes";
    CHECK(config_parse_line(&c, l1, &err) == 0 && !strcmp(c.listen, "192.168.1.1:6053"));
    CHECK(config_parse_line(&c, l2, &err) == 0 && c.n_allow == 2);
    CHECK(prefix_match(c.allow, c.n_allow, 0xC0A8000A) && prefix_match(c.allow, c.n_allow, 0xC0A80005));
    CHECK(!prefix_match(c.allow, c.n_allow, 0xC0A80101));
    CHECK(config_parse_line(&c, l3, &err) == 0 && c.n_devices == 1);
    CHECK(c.devices[0].addr == 0x001122334401ULL && !strcmp(c.devices[0].name, "Victron solar charger"));
    CHECK(config_device_name(&c, 0x001122334401ULL) != NULL);
    CHECK(config_parse_line(&c, l4, &err) == 0 && c.min_interval_ms == 2500);
    CHECK(config_parse_line(&c, l5, &err) == 0 && c.keepalive_ms == 120000);
    CHECK(config_parse_line(&c, l6, &err) == 0 && c.flush_ms == 500);
    CHECK(config_parse_line(&c, l7, &err) == 0);
    CHECK(config_parse_line(&c, l8, &err) == 0 && c.forward_all);

    char bad1[] = "device 00:11:22:33:44  short";
    char bad2[] = "colour blue";
    char bad3[] = "allow_from 10.0.0.0/33";
    CHECK(config_parse_line(&c, bad1, &err) < 0);
    CHECK(config_parse_line(&c, bad2, &err) < 0 && !strcmp(err, "unknown setting"));
    CHECK(config_parse_line(&c, bad3, &err) < 0);
}

static void test_raw_advert_encoding(void) {
    // One advert: address 0x0102, rssi -1, random address, data AA BB.
    struct advert a = {.addr = 0x0102, .rssi = -1, .addr_type = 1, .len = 2, .data = {0xAA, 0xBB}};
    const struct advert *list[] = {&a};
    uint8_t out[64];
    size_t n = api_encode_raw_adverts(list, 1, out, sizeof(out));
    const uint8_t want[] = {
        0x0A, 0x0B,              // field 1 (advertisements), 11 bytes
        0x08, 0x82, 0x02,        // address = 0x102
        0x10, 0x01,              // rssi = zigzag(-1) = 1
        0x18, 0x01,              // address_type = 1
        0x22, 0x02, 0xAA, 0xBB,  // data
    };
    CHECK(n == sizeof(want) && !memcmp(out, want, n));
}

int main(void) {
    test_crc();
    test_cobs_roundtrip();
    test_reader_skips_noise();
    test_hub_policy();
    test_hub_eviction();
    test_config();
    test_raw_advert_encoding();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all unit tests passed\n");
    return 0;
}
