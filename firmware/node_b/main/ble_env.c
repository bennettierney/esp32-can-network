#include "ble_env.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble";
static const char *DEVICE_NAME = "esp32-env";

// Custom 128 bit UUIDs. NimBLE wants the bytes in reverse (little endian) order.
//   service   4a5c0001-7f3e-4b8e-9d2a-1c6f0e8b3a55
//   reading   4a5c0002-7f3e-4b8e-9d2a-1c6f0e8b3a55
//   status    4a5c0003-7f3e-4b8e-9d2a-1c6f0e8b3a55
static const ble_uuid128_t SVC_UUID = BLE_UUID128_INIT(
    0x55, 0x3a, 0x8b, 0x0e, 0x6f, 0x1c, 0x2a, 0x9d, 0x8e, 0x4b, 0x3e, 0x7f, 0x01, 0x00, 0x5c, 0x4a);
static const ble_uuid128_t READING_UUID = BLE_UUID128_INIT(
    0x55, 0x3a, 0x8b, 0x0e, 0x6f, 0x1c, 0x2a, 0x9d, 0x8e, 0x4b, 0x3e, 0x7f, 0x02, 0x00, 0x5c, 0x4a);
static const ble_uuid128_t STATUS_UUID = BLE_UUID128_INIT(
    0x55, 0x3a, 0x8b, 0x0e, 0x6f, 0x1c, 0x2a, 0x9d, 0x8e, 0x4b, 0x3e, 0x7f, 0x03, 0x00, 0x5c, 0x4a);

// Human readable labels, shown by apps like nRF Connect (descriptor 0x2901).
static const char READING_DESCRIPTION[] =
    "T int16 0.01C, H uint16 0.01%, P uint32 Pa, little endian";
static const char STATUS_DESCRIPTION[] =
    "flags (bit0 node A online, bit1 sensor ok), node A uptime uint32 s, CAN errors uint32";

// The latest value of one characteristic. Written by node B's receive task, read by the
// NimBLE host task, so each one has its own lock.
struct char_value {
    uint8_t data[BLE_ENV_STATUS_LEN];  // big enough for either characteristic
    size_t len;                        // 0 until the first publish
    uint16_t handle;                   // filled in by NimBLE when the service is registered
    portMUX_TYPE lock;
};

static struct char_value s_reading = {.lock = portMUX_INITIALIZER_UNLOCKED};
static struct char_value s_status = {.lock = portMUX_INITIALIZER_UNLOCKED};

static uint8_t s_addr_type;

static void advertise(void);

// ---------------------------------------------------------------------------
// GATT service
// ---------------------------------------------------------------------------

// Called by NimBLE when a client reads a characteristic, and when a notification is built
// after ble_gatts_chr_updated. `arg` points at that characteristic's char_value.
static int value_access(uint16_t conn_handle, uint16_t attr_handle,
                        struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    struct char_value *v = arg;
    uint8_t copy[BLE_ENV_STATUS_LEN];
    size_t len;
    taskENTER_CRITICAL(&v->lock);
    len = v->len;
    memcpy(copy, v->data, len);
    taskEXIT_CRITICAL(&v->lock);

    // Nothing published yet: return an empty value rather than zeros that look real.
    int rc = os_mbuf_append(ctxt->om, copy, len);
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

// `arg` points at the description string.
static int description_access(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    const char *text = arg;
    int rc = os_mbuf_append(ctxt->om, text, strlen(text));
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static const struct ble_gatt_svc_def SERVICES[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &READING_UUID.u,
                .access_cb = value_access,
                .arg = &s_reading,
                .descriptors = (struct ble_gatt_dsc_def[]) {
                    {
                        .uuid = BLE_UUID16_DECLARE(0x2901),  // Characteristic User Description
                        .att_flags = BLE_ATT_F_READ,
                        .access_cb = description_access,
                        .arg = (void *)READING_DESCRIPTION,
                    },
                    {0},  // end of descriptors
                },
                // NOTIFY makes NimBLE add the client config descriptor (0x2902) that
                // clients write to subscribe.
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_reading.handle,
            },
            {
                .uuid = &STATUS_UUID.u,
                .access_cb = value_access,
                .arg = &s_status,
                .descriptors = (struct ble_gatt_dsc_def[]) {
                    {
                        .uuid = BLE_UUID16_DECLARE(0x2901),
                        .att_flags = BLE_ATT_F_READ,
                        .access_cb = description_access,
                        .arg = (void *)STATUS_DESCRIPTION,
                    },
                    {0},
                },
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_status.handle,
            },
            {0},  // end of characteristics
        },
    },
    {0},  // end of services
};

// ---------------------------------------------------------------------------
// Connection events
// ---------------------------------------------------------------------------

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "connect %s, status=%d",
                 event->connect.status == 0 ? "ok" : "failed", event->connect.status);
        if (event->connect.status != 0) {
            advertise();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnect, reason=0x%03x", event->disconnect.reason);
        advertise();  // only one client at a time, so become discoverable again
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "client %s notifications for %s",
                 event->subscribe.cur_notify ? "enabled" : "disabled",
                 event->subscribe.attr_handle == s_reading.handle ? "reading" : "status");
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU is %d", event->mtu.value);
        break;

    default:
        break;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Advertising
// ---------------------------------------------------------------------------

static void advertise(void)
{
    // An advertising packet holds 31 bytes. Flags, TX power and the name go in the
    // advertisement; the 16 byte service UUID goes in the scan response.
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.name = (const uint8_t *)DEVICE_NAME;
    fields.name_len = strlen(DEVICE_NAME);
    fields.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "setting advertisement data failed, rc=%d", rc);
        return;
    }

    struct ble_hs_adv_fields rsp = {0};
    rsp.uuids128 = &SVC_UUID;
    rsp.num_uuids128 = 1;
    rsp.uuids128_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGE(TAG, "setting scan response failed, rc=%d", rc);
        return;
    }

    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;  // anyone can connect
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;  // general discoverable
    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "starting advertising failed, rc=%d", rc);
        return;
    }
    ESP_LOGI(TAG, "advertising as \"%s\"", DEVICE_NAME);
}

// ---------------------------------------------------------------------------
// Stack lifecycle
// ---------------------------------------------------------------------------

// Runs once the host and the radio controller are in sync and ready.
static void on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &s_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "no usable BLE address, rc=%d", rc);
        return;
    }
    advertise();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "stack reset, reason=%d", reason);
}

// The NimBLE host runs in its own FreeRTOS task. nimble_port_run only returns on shutdown.
static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_env_init(void)
{
    // NVS holds the radio's PHY calibration data.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }

    err = nimble_port_init();
    if (err != ESP_OK) {
        return err;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    if (ble_gatts_count_cfg(SERVICES) != 0 || ble_gatts_add_svcs(SERVICES) != 0) {
        return ESP_FAIL;
    }
    if (ble_svc_gap_device_name_set(DEVICE_NAME) != 0) {
        return ESP_FAIL;
    }

    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

static void publish(struct char_value *v, const uint8_t *data, size_t len)
{
    taskENTER_CRITICAL(&v->lock);
    memcpy(v->data, data, len);
    v->len = len;
    taskEXIT_CRITICAL(&v->lock);

    if (v->handle == 0) {
        return;  // service not registered yet, the stack is still starting
    }
    // Tell NimBLE the value changed. It sends a notification to every connected client
    // that has subscribed, reading the new value through value_access. Does nothing if
    // nobody is subscribed.
    ble_gatts_chr_updated(v->handle);
}

void ble_env_publish_reading(const uint8_t payload[BLE_ENV_PAYLOAD_LEN])
{
    publish(&s_reading, payload, BLE_ENV_PAYLOAD_LEN);
}

void ble_env_publish_status(const uint8_t payload[BLE_ENV_STATUS_LEN])
{
    publish(&s_status, payload, BLE_ENV_STATUS_LEN);
}
