#include "linkcodec.h"

#include <string.h>

uint16_t link_crc16(const uint8_t *p, size_t n) {
    uint16_t crc = 0xFFFF;
    while (n--) {
        crc ^= (uint16_t)(*p++) << 8;
        for (int i = 0; i < 8; i++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

size_t link_cobs_encode(const uint8_t *in, size_t len, uint8_t *out) {
    size_t write = 1, code_idx = 0;
    uint8_t code = 1;
    for (size_t read = 0; read < len; read++) {
        if (in[read] == 0) {
            out[code_idx] = code;
            code_idx = write++;
            code = 1;
            continue;
        }
        out[write++] = in[read];
        if (++code == 0xFF) {
            out[code_idx] = code;
            code_idx = write++;
            code = 1;
        }
    }
    out[code_idx] = code;
    return write;
}

size_t link_cobs_decode(const uint8_t *in, size_t len, uint8_t *out, size_t cap) {
    size_t read = 0, write = 0;
    while (read < len) {
        uint8_t code = in[read++];
        if (code == 0 || read + code - 1 > len || write + code - 1 > cap) {
            return 0;
        }
        for (uint8_t i = 1; i < code; i++) {
            out[write++] = in[read++];
        }
        if (code < 0xFF && read < len) {
            if (write >= cap) {
                return 0;
            }
            out[write++] = 0;
        }
    }
    return write;
}

size_t link_frame_encode(uint8_t type, const uint8_t *payload, size_t len, uint8_t out[LINK_ENC_MAX]) {
    if (len > LINK_MAX_PAYLOAD) {
        return 0;
    }
    uint8_t raw[LINK_RAW_MAX];
    raw[0] = type;
    if (len) {
        memcpy(raw + 1, payload, len);
    }
    uint16_t crc = link_crc16(raw, len + 1);
    raw[len + 1] = crc & 0xFF;
    raw[len + 2] = crc >> 8;
    out[0] = 0;
    size_t n = 1 + link_cobs_encode(raw, len + 3, out + 1);
    out[n++] = 0;
    return n;
}

void link_reader_init(struct link_reader *r) {
    memset(r, 0, sizeof(*r));
}

int link_reader_feed(struct link_reader *r, uint8_t b, uint8_t *type, uint8_t *payload, size_t *len) {
    if (b != 0) {
        if (r->len < sizeof(r->acc)) {
            r->acc[r->len++] = b;
        } else {
            r->overflow = 1;
        }
        return 0;
    }
    if (r->len == 0) {
        return 0;  // empty frame between consecutive delimiters
    }
    int ok = 0;
    if (!r->overflow) {
        uint8_t raw[LINK_RAW_MAX];
        size_t m = link_cobs_decode(r->acc, r->len, raw, sizeof(raw));
        if (m >= 3 && link_crc16(raw, m - 2) == (uint16_t)(raw[m - 2] | raw[m - 1] << 8)) {
            *type = raw[0];
            *len = m - 3;
            memcpy(payload, raw + 1, m - 3);
            ok = 1;
        }
    }
    if (!ok) {
        r->bad++;
    }
    r->len = 0;
    r->overflow = 0;
    return ok;
}
