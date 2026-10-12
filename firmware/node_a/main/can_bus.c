#include "can_bus.h"

#include <inttypes.h>
#include <string.h>
#include "esp_log.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "can";

static const gpio_num_t CAN_TX_GPIO = GPIO_NUM_4;
static const gpio_num_t CAN_RX_GPIO = GPIO_NUM_5;
static const uint32_t CAN_BITRATE = 500000;
static const int SEND_TIMEOUT_MS = 100;

static twai_node_handle_t s_node;

// Updated from interrupt callbacks, read from tasks. 32 bit loads and stores are atomic
// on this chip, and volatile stops the compiler caching them.
static volatile uint32_t s_tx_ok;
static volatile uint32_t s_tx_failed;
static volatile uint32_t s_bus_errors;
static volatile uint32_t s_last_error_flags;

// Runs in interrupt context when a queued frame finishes, successfully or not.
static IRAM_ATTR bool on_tx_done(twai_node_handle_t node, const twai_tx_done_event_data_t *edata, void *ctx)
{
    if (edata->is_tx_success) {
        s_tx_ok++;
    } else {
        s_tx_failed++;
    }
    return false;  // no higher priority task was woken
}

// Runs in interrupt context on any bus error: bit error, missing ACK, stuff error, and so on.
// Wrong transceiver wiring shows up here as bit errors on every frame.
static IRAM_ATTR bool on_error(twai_node_handle_t node, const twai_error_event_data_t *edata, void *ctx)
{
    s_bus_errors++;
    s_last_error_flags = edata->err_flags.val;
    return false;
}

esp_err_t can_bus_init(bool self_test)
{
    twai_onchip_node_config_t cfg = {
        .io_cfg = {
            .tx = CAN_TX_GPIO,
            .rx = CAN_RX_GPIO,
            .quanta_clk_out = -1,
            .bus_off_indicator = -1,
        },
        .bit_timing = {
            .bitrate = CAN_BITRATE,
        },
        .fail_retry_cnt = 3,     // give up on a frame after 3 retries instead of retrying forever
        .tx_queue_depth = 4,
        .flags = {
            .enable_self_test = self_test,
        },
    };
    esp_err_t err = twai_new_node_onchip(&cfg, &s_node);
    if (err != ESP_OK) {
        return err;
    }

    twai_event_callbacks_t cbs = {
        .on_tx_done = on_tx_done,
        .on_error = on_error,
    };
    err = twai_node_register_event_callbacks(s_node, &cbs, NULL);
    if (err != ESP_OK) {
        return err;
    }

    err = twai_node_enable(s_node);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "TWAI started: TX GPIO %d, RX GPIO %d, %" PRIu32 " bps%s",
                 CAN_TX_GPIO, CAN_RX_GPIO, CAN_BITRATE, self_test ? ", self test mode" : "");
    }
    return err;
}

esp_err_t can_bus_send(uint32_t id, const uint8_t *data, uint8_t len)
{
    // Bus off: the controller shut itself down after too many errors. Start recovery and
    // skip this frame; recovery takes 128 sequences of 11 recessive bits on the bus.
    twai_node_status_t status;
    if (twai_node_get_info(s_node, &status, NULL) == ESP_OK && status.state == TWAI_ERROR_BUS_OFF) {
        ESP_LOGW(TAG, "bus off, recovering");
        twai_node_recover(s_node);
        return ESP_ERR_INVALID_STATE;
    }

    // The driver keeps a pointer to the data until the frame is sent, so use a copy that
    // stays valid until we have waited for completion below.
    uint8_t buf[8];
    if (len > sizeof(buf)) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(buf, data, len);

    twai_frame_t frame = {0};
    frame.header.id = id;      // standard 11 bit ID (ide = 0)
    frame.header.dlc = len;
    frame.buffer = buf;
    frame.buffer_len = len;

    // A frame that fails on the bus (bit error, no ACK) still returns ESP_OK from the calls
    // below; the failure only arrives through the on_tx_done callback. Compare the failure
    // count before and after to report what really happened.
    uint32_t failed_before = s_tx_failed;

    esp_err_t err = twai_node_transmit(s_node, &frame, SEND_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    err = twai_node_transmit_wait_all_done(s_node, SEND_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    return s_tx_failed != failed_before ? ESP_FAIL : ESP_OK;
}

void can_bus_log_status(void)
{
    twai_node_status_t status;
    twai_node_record_t record;
    if (twai_node_get_info(s_node, &status, &record) != ESP_OK) {
        return;
    }
    static const char *STATE_NAMES[] = {"active", "warning", "passive", "bus off"};
    const char *state = status.state < 4 ? STATE_NAMES[status.state] : "unknown";
    ESP_LOGI(TAG, "state=%s tx_ok=%" PRIu32 " tx_failed=%" PRIu32 " bus_errors=%" PRIu32
                  " last_error=0x%" PRIx32 " tec=%u rec=%u",
             state, s_tx_ok, s_tx_failed, s_bus_errors, s_last_error_flags,
             status.tx_error_count, status.rx_error_count);
}
