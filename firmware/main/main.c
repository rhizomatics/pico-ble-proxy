// BLE scanner for the host-side bleproxyd. Scans continuously and sends
// every advertisement / scan response to the host over USB. All filtering
// and Home Assistant logic lives on the host, so this rarely needs
// reflashing.

#include <string.h>

#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "link.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"

static const char *TAG = "bleproxy";

#define XIAO_RF_SWITCH_EN GPIO_NUM_3   // low = RF switch powered
#define XIAO_RF_ANT_SEL   GPIO_NUM_14  // low = ceramic, high = U.FL

#define MS_TO_SCAN_UNITS(ms) ((ms) * 1000 / 625)

static uint8_t own_addr_type;
static volatile uint32_t adv_seen, adv_dropped;

static void antenna_init(void) {
#if CONFIG_BLEPROXY_XIAO_RF_SWITCH
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << XIAO_RF_SWITCH_EN) | (1ULL << XIAO_RF_ANT_SEL),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(XIAO_RF_SWITCH_EN, 0);
#if CONFIG_BLEPROXY_EXTERNAL_ANTENNA
    gpio_set_level(XIAO_RF_ANT_SEL, 1);
    ESP_LOGI(TAG, "antenna: external U.FL");
#else
    gpio_set_level(XIAO_RF_ANT_SEL, 0);
    ESP_LOGI(TAG, "antenna: on-board ceramic");
#endif
#endif
}

static void send_hello(void) {
    uint8_t p[13 + 32];
    p[0] = 1;  // protocol version
    esp_read_mac(p + 1, ESP_MAC_BASE);
    esp_read_mac(p + 7, ESP_MAC_BT);
    const char *ver = esp_app_get_description()->version;
    size_t n = strnlen(ver, 32);
    memcpy(p + 13, ver, n);
    link_send(LINK_HELLO, p, 13 + n);
}

static void send_log(const char *msg) {
    link_send(LINK_LOG, (const uint8_t *)msg, strnlen(msg, LINK_MAX_PAYLOAD));
}

static void on_cmd(uint8_t type, const uint8_t *payload, size_t len) {
    switch (type) {
        case LINK_CMD_HELLO_REQ:
            send_hello();
            break;
        case LINK_CMD_REBOOT:
            send_log("rebooting on request");
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
            break;
    }
}

static int gap_event(struct ble_gap_event *event, void *arg);

static void start_scan(void) {
    struct ble_gap_disc_params params = {
        .itvl = MS_TO_SCAN_UNITS(CONFIG_BLEPROXY_SCAN_INTERVAL_MS),
        .window = MS_TO_SCAN_UNITS(CONFIG_BLEPROXY_SCAN_WINDOW_MS),
        .filter_policy = BLE_HCI_SCAN_FILT_NO_WL,
        .limited = 0,
#if CONFIG_BLEPROXY_ACTIVE_SCAN
        .passive = 0,
#else
        .passive = 1,
#endif
        // Every packet matters: Ruuvi/Victron change payload without
        // changing address, and the host does its own rate limiting.
        .filter_duplicates = 0,
    };
    int rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc failed: %d", rc);
        send_log("scan start failed");
    }
}

static int gap_event(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
        case BLE_GAP_EVENT_DISC: {
            const struct ble_gap_disc_desc *d = &event->disc;
            uint8_t p[9 + BLE_HS_ADV_MAX_SZ];
            for (int i = 0; i < 6; i++) {
                p[i] = d->addr.val[5 - i];  // NimBLE stores LSB first
            }
            p[6] = d->addr.type;
            p[7] = (uint8_t)d->rssi;
            p[8] = d->event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP ? 1 : 0;
            uint8_t n = d->length_data > BLE_HS_ADV_MAX_SZ ? BLE_HS_ADV_MAX_SZ : d->length_data;
            memcpy(p + 9, d->data, n);
            adv_seen++;
            if (!link_send(LINK_ADV, p, 9 + n)) {
                adv_dropped++;
            }
            return 0;
        }
        case BLE_GAP_EVENT_DISC_COMPLETE:
            start_scan();
            return 0;
        default:
            return 0;
    }
}

static void on_sync(void) {
    ESP_ERROR_CHECK(ble_hs_util_ensure_addr(0));
    ESP_ERROR_CHECK(ble_hs_id_infer_auto(0, &own_addr_type));
    ESP_LOGI(TAG, "BLE host synced, scanning");
    send_log("scanning");
    start_scan();
}

static void on_reset(int reason) {
    ESP_LOGW(TAG, "BLE host reset: %d", reason);
    send_log("BLE host reset");
}

static void host_task(void *param) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void status_task(void *arg) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        uint32_t v[4] = {
            (uint32_t)(esp_timer_get_time() / 1000000),
            adv_seen,
            adv_dropped,
            esp_get_free_heap_size(),
        };
        uint8_t p[16];
        for (int i = 0; i < 4; i++) {  // little-endian on the wire
            p[i * 4 + 0] = v[i];
            p[i * 4 + 1] = v[i] >> 8;
            p[i * 4 + 2] = v[i] >> 16;
            p[i * 4 + 3] = v[i] >> 24;
        }
        link_send(LINK_STATUS, p, sizeof(p));
    }
}

void app_main(void) {
    esp_err_t err = nvs_flash_init();  // PHY calibration data lives in NVS
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    antenna_init();
    link_init(on_cmd);
    send_hello();

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    nimble_port_freertos_init(host_task);

    xTaskCreate(status_task, "status", 2048, NULL, 3, NULL);
}
