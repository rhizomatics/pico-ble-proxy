#include "util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// No timestamps: procd/logread and journald add their own.
void logmsg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

int parse_prefix(const char *s, struct prefix *p) {
    char ip[INET_ADDRSTRLEN];
    const char *slash = strchr(s, '/');
    size_t n = slash ? (size_t)(slash - s) : strlen(s);
    if (n >= sizeof(ip)) {
        return -1;
    }
    memcpy(ip, s, n);
    ip[n] = 0;
    struct in_addr a;
    if (inet_pton(AF_INET, ip, &a) != 1) {
        return -1;
    }
    int bits = 32;
    if (slash) {
        char *end;
        long v = strtol(slash + 1, &end, 10);
        if (*end || v < 0 || v > 32) {
            return -1;
        }
        bits = (int)v;
    }
    p->mask = bits == 0 ? 0 : 0xFFFFFFFFu << (32 - bits);
    p->net = ntohl(a.s_addr) & p->mask;
    return 0;
}

int prefix_match(const struct prefix *ps, int n, uint32_t ip) {
    for (int i = 0; i < n; i++) {
        if ((ip & ps[i].mask) == ps[i].net) {
            return 1;
        }
    }
    return 0;
}

int parse_hostport(const char *s, struct sockaddr_in *sa) {
    const char *colon = strrchr(s, ':');
    if (!colon) {
        return -1;
    }
    char host[INET_ADDRSTRLEN] = "0.0.0.0";
    size_t n = (size_t)(colon - s);
    if (n >= sizeof(host)) {
        return -1;
    }
    if (n > 0) {
        memcpy(host, s, n);
        host[n] = 0;
    }
    char *end;
    long port = strtol(colon + 1, &end, 10);
    if (*end || port <= 0 || port > 65535) {
        return -1;
    }
    memset(sa, 0, sizeof(*sa));
    sa->sin_family = AF_INET;
    sa->sin_port = htons((uint16_t)port);
    return inet_pton(AF_INET, host, &sa->sin_addr) == 1 ? 0 : -1;
}

int set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL);
    return fl < 0 ? -1 : fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

int listen_tcp(const char *addr) {
    struct sockaddr_in sa;
    if (parse_hostport(addr, &sa) < 0) {
        errno = EINVAL;
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(fd, 4) < 0 || set_nonblock(fd) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

int write_all(int fd, const void *buf, size_t len, int timeout_ms) {
    const uint8_t *p = buf;
    int64_t deadline = now_ms() + timeout_ms;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n > 0) {
            p += n;
            len -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            return -1;
        }
        int64_t left = deadline - now_ms();
        if (left <= 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        struct pollfd pfd = {.fd = fd, .events = POLLOUT};
        poll(&pfd, 1, (int)left);
    }
    return 0;
}

void fmt_mac(uint64_t a, char out[18]) {
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", (unsigned)(a >> 40) & 0xFF, (unsigned)(a >> 32) & 0xFF,
             (unsigned)(a >> 24) & 0xFF, (unsigned)(a >> 16) & 0xFF, (unsigned)(a >> 8) & 0xFF, (unsigned)a & 0xFF);
}

void fmt_mac_bytes(const uint8_t b[6], char out[18]) {
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", b[0], b[1], b[2], b[3], b[4], b[5]);
}

int parse_mac(const char *s, uint64_t *out) {
    uint64_t v = 0;
    for (int i = 0; i < 6; i++) {
        char *end;
        unsigned long b = strtoul(s, &end, 16);
        if (end == s || end - s > 2 || b > 0xFF || (i < 5 && *end != ':') || (i == 5 && *end)) {
            return -1;
        }
        v = v << 8 | b;
        s = end + 1;
    }
    *out = v;
    return 0;
}
