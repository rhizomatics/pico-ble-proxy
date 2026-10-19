#pragma once

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>

#ifndef VERSION
#define VERSION "dev"
#endif

int64_t now_ms(void);

void logmsg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// IPv4 prefix in host byte order.
struct prefix {
    uint32_t net, mask;
};

// Accepts "a.b.c.d" or "a.b.c.d/len".
int parse_prefix(const char *s, struct prefix *p);
int prefix_match(const struct prefix *ps, int n, uint32_t ip_host);

// Accepts "host:port" or ":port" (IPv4).
int parse_hostport(const char *s, struct sockaddr_in *sa);
// Returns a non-blocking listening socket, or -1.
int listen_tcp(const char *addr);
int set_nonblock(int fd);
// Writes all of buf to a possibly non-blocking fd, waiting up to timeout_ms.
int write_all(int fd, const void *buf, size_t len, int timeout_ms);

// MACs are 48-bit integers, most significant byte first as printed.
void fmt_mac(uint64_t a, char out[18]);
void fmt_mac_bytes(const uint8_t b[6], char out[18]);
int parse_mac(const char *s, uint64_t *out);
