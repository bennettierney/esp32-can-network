// Node B: gateway node.
//
// Receives CAN frames from node A, decodes them, and tracks node A's health from its
// heartbeat. Next step: forward the readings over BLE.
//
//   TWAI receive interrupt ──CanFrame──▶ [rx queue] ──▶ rx_task (decode, log, health check)
//
// Wiring: GPIO 4 to CAN Pal TX, GPIO 5 to CAN Pal RX (TX to TX, RX to RX). 500 kbps.

#include <atomic>
#include <inttypes.h>
#include <string.h>
#include "esp_log.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "can_frame.h"

static const char *TAG = "node_b";

static constexpr gpio_num_t CAN_TX_GPIO = GPIO_NUM_4;
static constexpr gpio_num_t CAN_RX_GPIO = GPIO_NUM_5;
static constexpr uint32_t CAN_BITRATE = 500000;

// Node A sends a heartbeat every second. Three missed in a row means it is offline.
static constexpr TickType_t HEARTBEAT_TIMEOUT = pdMS_TO_TICKS(3000);
static constexpr TickType_t STATUS_PERIOD = pdMS_TO_TICKS(10000);
static constexpr UBaseType_t RX_QUEUE_LENGTH = 16;

static twai_node_handle_t s_node;
static QueueHandle_t s_rx_queue;

// Updated from interrupt callbacks, read from the task. std::atomic makes each update a
// single indivisible operation, so the task never sees a half written value.
static std::atomic<uint32_t> s_rx_dropped{0};  // frames lost because the queue was full
static std::atomic<uint32_t> s_bus_errors{0};

// ---------------------------------------------------------------------------
// Interrupt callbacks. Keep them short: copy the frame out and hand it to a task.
// ---------------------------------------------------------------------------

static IRAM_ATTR bool on_rx_done(twai_node_handle_t node, const twai_rx_done_event_data_t *edata, void *ctx)
{
    uint8_t buf[8];
    twai_frame_t rx = {};
    rx.buffer = buf;
    rx.buffer_len = sizeof(buf);
    if (twai_node_receive_from_isr(node, &rx) != ESP_OK) {
        return false;
    }

    CanFrame f = {};
    f.id = rx.header.id;
    f.len = rx.header.dlc > 8 ? 8 : rx.header.dlc;
    memcpy(f.data, buf, f.len);

    BaseType_t higher_priority_woken = pdFALSE;
    if (xQueueSendFromISR(s_rx_queue, &f, &higher_priority_woken) != pdTRUE) {
        s_rx_dropped++;
    }
    return higher_priority_woken == pdTRUE;  // tells the driver to yield when the ISR ends
}

static IRAM_ATTR bool on_error(twai_node_handle_t node, const twai_error_event_data_t *edata, void *ctx)
{
    s_bus_errors++;
    return false;
}

// ---------------------------------------------------------------------------
// CAN setup
// ---------------------------------------------------------------------------

static esp_err_t can_init()
{
    // Normal mode: node B acknowledges every frame it receives. Node A needs that ACK
    // once it leaves self test mode.
    twai_onchip_node_config_t cfg = {};
    cfg.io_cfg.tx = CAN_TX_GPIO;
    cfg.io_cfg.rx = CAN_RX_GPIO;
    cfg.io_cfg.quanta_clk_out = GPIO_NUM_NC;
    cfg.io_cfg.bus_off_indicator = GPIO_NUM_NC;
    cfg.bit_timing.bitrate = CAN_BITRATE;
    cfg.fail_retry_cnt = 3;
    cfg.tx_queue_depth = 4;

    esp_err_t err = twai_new_node_onchip(&cfg, &s_node);
    if (err != ESP_OK) {
        return err;
    }

    twai_event_callbacks_t cbs = {};
    cbs.on_rx_done = on_rx_done;
    cbs.on_error = on_error;
    err = twai_node_register_event_callbacks(s_node, &cbs, nullptr);
    if (err != ESP_OK) {
        return err;
    }
    err = twai_node_enable(s_node);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "TWAI started: TX GPIO %d, RX GPIO %d, %" PRIu32 " bps", CAN_TX_GPIO, CAN_RX_GPIO, CAN_BITRATE);
    }
    return err;
}

// ---------------------------------------------------------------------------
// Receive task
// ---------------------------------------------------------------------------

static void rx_task(void *arg)
{
    bool node_a_online = false;
    bool sensor_ok = false;
    TickType_t last_heartbeat = 0;
    uint32_t readings = 0, heartbeats = 0, unknown = 0;
    TickType_t next_status = xTaskGetTickCount() + STATUS_PERIOD;

    while (true) {
        // Wake at least twice a second, even with no traffic, to run the health check.
        CanFrame f;
        if (xQueueReceive(s_rx_queue, &f, pdMS_TO_TICKS(500)) == pdTRUE) {
            if (f.id == CAN_ID_ENV_READING && f.len == 8) {
                readings++;
                EnvReading r = can_unpack_reading(f);
                ESP_LOGI(TAG, "RX 0x%03" PRIX32 "  T=%" PRId32 ".%02" PRId32 " C  H=%" PRIu32 ".%02" PRIu32
                              " %%  P=%" PRIu32 " Pa",
                         f.id, r.temp_c100 / 100, r.temp_c100 % 100, r.hum_c100 / 100, r.hum_c100 % 100,
                         r.press_pa);
            } else if (f.id == CAN_ID_HEARTBEAT_NODE_A && f.len == 8) {
                heartbeats++;
                last_heartbeat = xTaskGetTickCount();
                Heartbeat h = can_unpack_heartbeat(f);
                if (!node_a_online) {
                    ESP_LOGI(TAG, "node A online (uptime %" PRIu32 " s)", h.uptime_s);
                    node_a_online = true;
                }
                if (h.sensor_ok != sensor_ok) {
                    ESP_LOGI(TAG, "node A sensor %s", h.sensor_ok ? "ok" : "DOWN");
                    sensor_ok = h.sensor_ok;
                }
            } else {
                unknown++;
                ESP_LOGW(TAG, "unexpected frame id=0x%03" PRIX32 " len=%u", f.id, f.len);
            }
        }

        // Health check: no heartbeat for HEARTBEAT_TIMEOUT means node A is offline.
        if (node_a_online && xTaskGetTickCount() - last_heartbeat > HEARTBEAT_TIMEOUT) {
            ESP_LOGW(TAG, "node A OFFLINE: no heartbeat for %" PRIu32 " ms", (uint32_t)pdTICKS_TO_MS(HEARTBEAT_TIMEOUT));
            node_a_online = false;
        }

        if ((int32_t)(xTaskGetTickCount() - next_status) >= 0) {
            next_status += STATUS_PERIOD;
            twai_node_status_t st;
            twai_node_get_info(s_node, &st, nullptr);
            ESP_LOGI(TAG, "status: node A %s, sensor %s | readings=%" PRIu32 " heartbeats=%" PRIu32
                          " unknown=%" PRIu32 " dropped=%" PRIu32 " bus_errors=%" PRIu32 " tec=%u rec=%u",
                     node_a_online ? "online" : "offline", sensor_ok ? "ok" : "down",
                     readings, heartbeats, unknown, s_rx_dropped.load(), s_bus_errors.load(),
                     st.tx_error_count, st.rx_error_count);
        }
    }
}

extern "C" void app_main(void)
{
    s_rx_queue = xQueueCreate(RX_QUEUE_LENGTH, sizeof(CanFrame));
    if (s_rx_queue == nullptr) {
        ESP_LOGE(TAG, "failed to create rx queue");
        abort();
    }
    ESP_ERROR_CHECK(can_init());
    if (xTaskCreate(rx_task, "rx", 4096, nullptr, 5, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "failed to start rx task");
        abort();
    }
}
