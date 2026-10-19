// Serial link framing shared by the ESP32 firmware and bleproxyd.
// See docs/serial-protocol.md.
#pragma once

#include <stddef.h>
#include <stdint.h>

#define LINK_HELLO 0x01
#define LINK_ADV 0x02
#define LINK_STATUS 0x03
#define LINK_LOG 0x04

#define LINK_CMD_HELLO_REQ 0x81
#define LINK_CMD_REBOOT 0x82

#define LINK_MAX_PAYLOAD 128
#define LINK_RAW_MAX (LINK_MAX_PAYLOAD + 3)  // type + payload + crc16
// Leading delimiter + COBS overhead + trailing delimiter.
#define LINK_ENC_MAX (LINK_RAW_MAX + LINK_RAW_MAX / 254 + 1 + 2)

// CRC-16/CCITT-FALSE.
uint16_t link_crc16(const uint8_t *p, size_t n);

// COBS without delimiter. encode returns bytes written (out needs len + len/254 + 1).
size_t link_cobs_encode(const uint8_t *in, size_t len, uint8_t *out);
// Returns decoded length, or 0 if malformed or larger than cap.
size_t link_cobs_decode(const uint8_t *in, size_t len, uint8_t *out, size_t cap);

// Builds a complete wire frame (delimited on both sides). Returns its length,
// or 0 if the payload is too large.
size_t link_frame_encode(uint8_t type, const uint8_t *payload, size_t len, uint8_t out[LINK_ENC_MAX]);

// Splits a byte stream into frames, dropping corrupt ones.
struct link_reader {
    uint8_t acc[LINK_ENC_MAX];
    size_t len;
    int overflow;
    unsigned long bad;  // frames dropped for COBS, CRC or size errors
};

void link_reader_init(struct link_reader *r);
// Feeds one byte. Returns 1 when it completes a valid frame, filling
// type/payload/len (payload needs LINK_MAX_PAYLOAD bytes).
int link_reader_feed(struct link_reader *r, uint8_t b, uint8_t *type, uint8_t *payload, size_t *len);
