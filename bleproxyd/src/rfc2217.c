#include "rfc2217.h"

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "serial.h"
#include "util.h"

enum {
    IAC = 255,
    DONT = 254,
    DO = 253,
    WONT = 252,
    WILL = 251,
    SB = 250,
    SE = 240,

    OPT_BINARY = 0,
    OPT_SGA = 3,
    OPT_COM_PORT = 44,

    SET_BAUDRATE = 1,
    SET_CONTROL = 5,
    SERVER_OFFSET = 100,  // server replies use the client command + 100

    CTL_DTR_ON = 8,
    CTL_DTR_OFF = 9,
    CTL_RTS_ON = 11,
    CTL_RTS_OFF = 12,
};

enum parse_state {
    ST_DATA,
    ST_IAC,
    ST_OPT,
    ST_SB,
    ST_SB_IAC
};

struct session {
    int cfd, sfd;
    const char *pattern;
    int dtr, rts;
    uint8_t baud[4];
    enum parse_state st;
    uint8_t opt_cmd;
    uint8_t sb[64];
    size_t sb_len;
    uint8_t ser_out[8192];
    size_t ser_len;
};

static int reopen(struct session *s) {
    if (s->sfd >= 0) {
        close(s->sfd);
    }
    char name[128];
    for (int i = 0; i < 20; i++) {
        s->sfd = serial_open(s->pattern, name, sizeof(name));
        if (s->sfd >= 0) {
            if (s->dtr >= 0 || s->rts >= 0) {
                serial_set_lines(s->sfd, s->dtr, s->rts);
            }
            return 0;
        }
        usleep(250 * 1000);
    }
    return -1;
}

static int net_write(struct session *s, const uint8_t *b, size_t n) {
    return write_all(s->cfd, b, n, 10000);
}

static void flush_serial(struct session *s) {
    if (s->ser_len == 0) {
        return;
    }
    if (write_all(s->sfd, s->ser_out, s->ser_len, 2000) < 0) {
        logmsg("flash-bridge: serial write: %s", strerror(errno));
    }
    s->ser_len = 0;
}

static int supported(uint8_t opt) {
    return opt == OPT_BINARY || opt == OPT_SGA || opt == OPT_COM_PORT;
}

static void negotiate(struct session *s, uint8_t cmd, uint8_t opt) {
    uint8_t r[3] = {IAC, 0, opt};
    if (cmd == WILL) {
        r[1] = supported(opt) ? DO : DONT;
    } else if (cmd == DO) {
        r[1] = supported(opt) ? WILL : WONT;
    } else {
        return;  // WONT/DONT need no reply for options we never enabled
    }
    net_write(s, r, 3);
}

static void subnegotiate(struct session *s) {
    if (s->sb_len < 2 || s->sb[0] != OPT_COM_PORT) {
        return;
    }
    uint8_t cmd = s->sb[1];
    uint8_t *val = s->sb + 2;
    size_t vlen = s->sb_len - 2;
    uint8_t reply[8];
    size_t rlen = vlen < sizeof(reply) ? vlen : sizeof(reply);
    memcpy(reply, val, rlen);

    if (cmd == SET_BAUDRATE && vlen == 4) {
        if (val[0] | val[1] | val[2] | val[3]) {
            memcpy(s->baud, val, 4);  // USB CDC ignores baud; just remember it
        }
        memcpy(reply, s->baud, 4);
        rlen = 4;
    } else if (cmd == SET_CONTROL && vlen == 1) {
        flush_serial(s);  // control changes must happen after preceding data
        switch (val[0]) {
            case CTL_DTR_ON:
            case CTL_DTR_OFF:
                s->dtr = val[0] == CTL_DTR_ON;
                serial_set_lines(s->sfd, s->dtr, -1);
                break;
            case CTL_RTS_ON:
            case CTL_RTS_OFF:
                s->rts = val[0] == CTL_RTS_ON;
                serial_set_lines(s->sfd, -1, s->rts);
                break;
        }
    }
    // Data size, parity, stop bits, purge, masks: acknowledge as requested.
    uint8_t out[32];
    size_t n = 0;
    out[n++] = IAC;
    out[n++] = SB;
    out[n++] = OPT_COM_PORT;
    out[n++] = (uint8_t)(cmd + SERVER_OFFSET);
    for (size_t i = 0; i < rlen; i++) {
        out[n++] = reply[i];
        if (reply[i] == IAC) {
            out[n++] = IAC;
        }
    }
    out[n++] = IAC;
    out[n++] = SE;
    net_write(s, out, n);
}

static void from_client(struct session *s, const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint8_t c = b[i];
        switch (s->st) {
            case ST_DATA:
                if (c == IAC) {
                    s->st = ST_IAC;
                } else {
                    s->ser_out[s->ser_len++] = c;
                    if (s->ser_len == sizeof(s->ser_out)) {
                        flush_serial(s);
                    }
                }
                break;
            case ST_IAC:
                if (c == IAC) {
                    s->ser_out[s->ser_len++] = IAC;
                    if (s->ser_len == sizeof(s->ser_out)) {
                        flush_serial(s);
                    }
                    s->st = ST_DATA;
                } else if (c == WILL || c == WONT || c == DO || c == DONT) {
                    s->opt_cmd = c;
                    s->st = ST_OPT;
                } else if (c == SB) {
                    s->sb_len = 0;
                    s->st = ST_SB;
                } else {
                    s->st = ST_DATA;  // NOP, GA etc.
                }
                break;
            case ST_OPT:
                negotiate(s, s->opt_cmd, c);
                s->st = ST_DATA;
                break;
            case ST_SB:
                if (c == IAC) {
                    s->st = ST_SB_IAC;
                } else if (s->sb_len < sizeof(s->sb)) {
                    s->sb[s->sb_len++] = c;
                }
                break;
            case ST_SB_IAC:
                if (c == SE) {
                    subnegotiate(s);
                    s->st = ST_DATA;
                } else {
                    if (s->sb_len < sizeof(s->sb)) {
                        s->sb[s->sb_len++] = c;  // IAC IAC inside SB
                    }
                    s->st = ST_SB;
                }
                break;
        }
    }
    flush_serial(s);
}

static int from_serial(struct session *s) {
    uint8_t buf[4096], esc[8192];
    ssize_t n = read(s->sfd, buf, sizeof(buf));
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        return 0;
    }
    if (n <= 0) {
        logmsg("flash-bridge: serial %s, reopening", n == 0 ? "closed" : strerror(errno));
        if (reopen(s) < 0) {
            logmsg("flash-bridge: serial port gone");
            return -1;
        }
        return 0;
    }
    size_t m = 0;
    for (ssize_t i = 0; i < n; i++) {
        esc[m++] = buf[i];
        if (buf[i] == IAC) {
            esc[m++] = IAC;
        }
    }
    return net_write(s, esc, m);
}

int rfc2217_session(int cfd, const char *pattern) {
    static struct session s;
    memset(&s, 0, sizeof(s));
    s.cfd = cfd;
    s.sfd = -1;
    s.pattern = pattern;
    s.dtr = s.rts = -1;
    memcpy(s.baud, (uint8_t[]){0, 1, 0xC2, 0}, 4);  // 115200
    if (reopen(&s) < 0) {
        logmsg("flash-bridge: can't open %s: %s", pattern, strerror(errno));
        return -1;
    }
    // Ask for binary both ways and offer COM-PORT-OPTION up front.
    static const uint8_t hello[] = {IAC, WILL, OPT_BINARY, IAC, DO, OPT_BINARY, IAC, WILL, OPT_COM_PORT};
    net_write(&s, hello, sizeof(hello));

    int rc = 0;
    for (;;) {
        struct pollfd p[2] = {{.fd = cfd, .events = POLLIN}, {.fd = s.sfd, .events = POLLIN}};
        if (poll(p, 2, 1000) < 0) {
            if (errno == EINTR) {
                continue;
            }
            rc = -1;
            break;
        }
        if (p[1].revents) {
            if (from_serial(&s) < 0) {
                rc = -1;
                break;
            }
        }
        if (p[0].revents) {
            uint8_t buf[4096];
            ssize_t n = read(cfd, buf, sizeof(buf));
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
                continue;
            }
            if (n <= 0) {
                break;  // client went away
            }
            from_client(&s, buf, (size_t)n);
        }
    }
    close(s.sfd);
    return rc;
}
