#include "serial.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

#include "util.h"

static int open_tcp(const char *addr) {
    struct sockaddr_in sa;
    if (parse_hostport(addr, &sa) < 0) {
        errno = EINVAL;
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    set_nonblock(fd);
    return fd;
}

static int open_tty(const char *path) {
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    struct termios t;
    if (tcgetattr(fd, &t) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    cfmakeraw(&t);
    // No flow control, and no hang-up on close: dropping DTR/RTS could
    // reset the chip.
    t.c_cflag &= ~(tcflag_t)(CRTSCTS | HUPCL);
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    cfsetispeed(&t, B115200);  // ignored by USB CDC
    cfsetospeed(&t, B115200);
    if (tcsetattr(fd, TCSANOW, &t) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

int serial_open(const char *pattern, char *name, size_t namelen) {
    if (!strncmp(pattern, "tcp://", 6)) {
        snprintf(name, namelen, "%s", pattern);
        return open_tcp(pattern + 6);
    }
    if (!strpbrk(pattern, "*?[")) {
        snprintf(name, namelen, "%s", pattern);
        return open_tty(pattern);
    }
    glob_t g;
    if (glob(pattern, 0, NULL, &g) != 0 || g.gl_pathc == 0) {
        globfree(&g);
        errno = ENOENT;
        return -1;
    }
    snprintf(name, namelen, "%s", g.gl_pathv[0]);  // glob sorts its results
    globfree(&g);
    return open_tty(name);
}

int serial_set_lines(int fd, int dtr, int rts) {
    int rc = 0;
    if (dtr >= 0) {
        int bit = TIOCM_DTR;
        rc |= ioctl(fd, dtr ? TIOCMBIS : TIOCMBIC, &bit);
    }
    if (rts >= 0) {
        int bit = TIOCM_RTS;
        rc |= ioctl(fd, rts ? TIOCMBIS : TIOCMBIC, &bit);
    }
    return rc;
}
