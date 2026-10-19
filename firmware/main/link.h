// Framed serial link to the host. See docs/serial-protocol.md.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "linkcodec.h"

typedef void (*link_cmd_handler_t)(uint8_t type, const uint8_t *payload, size_t len);

// Installs the USB-Serial-JTAG driver and starts the TX/RX tasks.
void link_init(link_cmd_handler_t on_cmd);

// Queues a frame. Never blocks; returns false (and drops the frame) when the
// queue is full, e.g. while nothing on the host is reading.
bool link_send(uint8_t type, const uint8_t *payload, size_t len);
