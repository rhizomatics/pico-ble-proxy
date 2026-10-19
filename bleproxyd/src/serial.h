// The ESP32's USB CDC-ACM port in raw mode.
#pragma once

#include <stddef.h>

// Opens the first device matching pattern (glob allowed), or connects to
// tcp://host:port for development. Returns a non-blocking fd, or -1 with
// errno set. The resolved name is written to name.
int serial_open(const char *pattern, char *name, size_t namelen);

// Sets DTR/RTS (-1 leaves a line unchanged). On the C6's USB-Serial-JTAG,
// RTS high with DTR low resets the chip, which esptool uses.
int serial_set_lines(int fd, int dtr, int rts);
