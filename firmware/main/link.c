#include "link.h"

#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"

static RingbufHandle_t txq;
static link_cmd_handler_t cmd_handler;

bool link_send(uint8_t type, const uint8_t *payload, size_t len) {
    if (txq == NULL) {
        return false;
    }
    uint8_t enc[LINK_ENC_MAX];
    size_t n = link_frame_encode(type, payload, len, enc);
    return n > 0 && xRingbufferSend(txq, enc, n, 0) == pdTRUE;
}

static void tx_task(void *arg) {
    for (;;) {
        size_t n;
        uint8_t *item = xRingbufferReceive(txq, &n, portMAX_DELAY);
        if (item == NULL) {
            continue;
        }
        // Blocks while the host isn't reading; the ring buffer absorbs that
        // and link_send() starts dropping once it's full.
        usb_serial_jtag_write_bytes(item, n, portMAX_DELAY);
        vRingbufferReturnItem(txq, item);
    }
}

static void rx_task(void *arg) {
    static struct link_reader reader;
    uint8_t buf[64], payload[LINK_MAX_PAYLOAD], type;
    size_t len;
    link_reader_init(&reader);
    for (;;) {
        int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), portMAX_DELAY);
        for (int i = 0; i < n; i++) {
            if (link_reader_feed(&reader, buf[i], &type, payload, &len) && cmd_handler) {
                cmd_handler(type, payload, len);
            }
        }
    }
}

void link_init(link_cmd_handler_t on_cmd) {
    cmd_handler = on_cmd;
    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = 4096,
        .rx_buffer_size = 256,
    };
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
    // ~16 KB of queued frames: a few seconds of a busy marina.
    txq = xRingbufferCreate(16 * 1024, RINGBUF_TYPE_NOSPLIT);
    xTaskCreate(tx_task, "link_tx", 3072, NULL, 5, NULL);
    xTaskCreate(rx_task, "link_rx", 3072, NULL, 5, NULL);
}
