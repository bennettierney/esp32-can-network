// Node A: sensor node.
//
// Three FreeRTOS tasks connected by two queues:
//
//   sensor_task ──Sample──▶ [sample queue] ──▶ processing_task ──Outgoing──▶ [frame queue] ──▶ comms_task
//        ▲
//   BOOT button ISR (task notification: "sample now")
//
// sensor_task      owns the I2C bus and the BME280. Samples once a second, or right away
//                  when the button is pressed. Handles bus recovery.
// processing_task  range checks each reading and packs it into the 8 byte CAN layout.
// comms_task       sends each frame on the CAN bus (can_bus), publishes it over BLE
//                  (ble_env), and logs it. Also sends a CAN heartbeat every second.

#include <atomic>
#include <inttypes.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "ble_env.h"
#include "bme280.h"
#include "can_bus.h"
#include "can_frame.h"

static const char *TAG = "node_a";

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
static constexpr TickType_t SAMPLE_PERIOD = pdMS_TO_TICKS(1000);
static constexpr int MAX_CONSECUTIVE_FAILURES = 3;

// Self test: CAN frames go out without needing an acknowledgment from another node, so
// node A can check its own transceiver wiring alone. Set to false once node B is on the bus,
// so a missing receiver shows up as an error.
static constexpr bool CAN_SELF_TEST = false;

// Each queue holds 8 items, about 8 seconds of readings. Enough to ride out a brief
// stall downstream without growing memory use.
static constexpr UBaseType_t QUEUE_LENGTH = 8;

// Higher number wins when two tasks are ready at the same time. The sensor task is
// highest so sampling stays on schedule even if the other tasks are busy.
static constexpr UBaseType_t PRIO_SENSOR = 5;
static constexpr UBaseType_t PRIO_PROCESSING = 4;
static constexpr UBaseType_t PRIO_COMMS = 3;
static constexpr uint32_t STACK_BYTES = 4096;  // ESP-IDF measures task stacks in bytes

// BOOT button: GPIO 0, pulled up, reads low while pressed.
static constexpr gpio_num_t BUTTON_GPIO = GPIO_NUM_0;
static constexpr int64_t DEBOUNCE_US = 200 * 1000;

static constexpr TickType_t HEARTBEAT_PERIOD = pdMS_TO_TICKS(1000);

// ---------------------------------------------------------------------------
// Messages passed between tasks. Queues copy these by value.
// ---------------------------------------------------------------------------
struct Sample {
    uint32_t seq;        // increments per successful reading, shows gaps if any are dropped
    bool on_demand;      // true if triggered by the button, false if scheduled
    BmeReading reading;
};

struct Outgoing {
    uint32_t seq;
    bool on_demand;
    CanFrame frame;
};

static QueueHandle_t s_sample_queue;
static QueueHandle_t s_frame_queue;
static TaskHandle_t s_sensor_task;  // the button ISR needs this to notify the task

// Set by the sensor task, read by the comms task for the heartbeat. Atomic because two
// tasks touch it.
static std::atomic<bool> s_sensor_ok{false};

// Send without blocking. If the queue is full, drop the oldest item to make room:
// for telemetry, a fresh reading is worth more than a stale one, and the producer
// must never stall waiting on a slow consumer.
template <typename T>
static void send_latest(QueueHandle_t q, const T &item, const char *name)
{
    if (xQueueSend(q, &item, 0) == pdTRUE) {
        return;
    }
    T discarded;
    xQueueReceive(q, &discarded, 0);
    ESP_LOGW(TAG, "%s queue full, dropped oldest item", name);
    xQueueSend(q, &item, 0);
}

// ---------------------------------------------------------------------------
// Interrupt: BOOT button
// ---------------------------------------------------------------------------

// Runs in interrupt context. Rules: keep it short, never block, never log, and only
// use the FromISR versions of FreeRTOS calls. All it does is wake the sensor task.
static void IRAM_ATTR button_isr(void *arg)
{
    // A mechanical button bounces, firing several edges per press. Ignore edges that
    // arrive within DEBOUNCE_US of the last accepted one.
    static int64_t last_press_us = 0;
    int64_t now_us = esp_timer_get_time();
    if (now_us - last_press_us < DEBOUNCE_US) {
        return;
    }
    last_press_us = now_us;

    // Increment the sensor task's notification count. If that unblocks a task with a
    // higher priority than the one we interrupted, switch to it as soon as the ISR ends.
    BaseType_t higher_priority_woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_sensor_task, &higher_priority_woken);
    portYIELD_FROM_ISR(higher_priority_woken);
}

static void button_init()
{
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << BUTTON_GPIO;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    io.intr_type = GPIO_INTR_NEGEDGE;  // falling edge: the moment the button goes down
    ESP_ERROR_CHECK(gpio_config(&io));

    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(BUTTON_GPIO, button_isr, nullptr));
}

// ---------------------------------------------------------------------------
// Tasks
// ---------------------------------------------------------------------------

static void sensor_task(void *arg)
{
    ESP_ERROR_CHECK(bme280_bus_init());
    bme280_scan_bus();

    bool ready = false;  // true once bme280_init has succeeded
    int failures = 0;    // consecutive failed reads
    uint32_t seq = 0;
    TickType_t next_sample = xTaskGetTickCount() + SAMPLE_PERIOD;

    while (true) {
        // Sleep until the next scheduled sample, or until the button ISR notifies us,
        // whichever comes first. Signed math keeps this correct when the tick count wraps.
        int32_t remaining = (int32_t)(next_sample - xTaskGetTickCount());
        TickType_t wait = remaining > 0 ? (TickType_t)remaining : 0;
        bool on_demand = ulTaskNotifyTake(pdTRUE, wait) > 0;

        if (on_demand) {
            ESP_LOGI(TAG, "button pressed, sampling now");
        } else {
            // Advance by exactly one period so the schedule does not drift. If we fell
            // far behind (a long recovery), resync instead of firing a burst to catch up.
            next_sample += SAMPLE_PERIOD;
            if ((int32_t)(next_sample - xTaskGetTickCount()) <= 0) {
                next_sample = xTaskGetTickCount() + SAMPLE_PERIOD;
            }
        }

        // Bring the sensor up, at boot or after a recovery. Keep retrying instead of
        // giving up, so a loose wire or a glitch at power on is not fatal.
        if (!ready) {
            esp_err_t err = bme280_init();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "sensor init failed: %s, recovering I2C bus", esp_err_to_name(err));
                s_sensor_ok = false;
                bme280_recover();
                continue;
            }
            ESP_LOGI(TAG, "sensor ready");
            ready = true;
            failures = 0;
        }

        BmeReading r;
        esp_err_t err = bme280_read(&r);
        if (err == ESP_OK) {
            failures = 0;
            s_sensor_ok = true;
            Sample s = {seq++, on_demand, r};
            send_latest(s_sample_queue, s, "sample");
        } else if (err == ESP_ERR_NOT_FINISHED) {
            // First measurement after configure is not ready yet. Not a bus problem.
            ESP_LOGD(TAG, "measurement not ready yet");
        } else {
            failures++;
            s_sensor_ok = false;
            ESP_LOGE(TAG, "read failed (%d in a row): %s", failures, esp_err_to_name(err));
            if (failures >= MAX_CONSECUTIVE_FAILURES) {
                // The bus or the I2C controller is stuck, for example after a glitch on
                // SDA or SCL mid transfer. Rebuild the bus, then re-initialize the sensor.
                ESP_LOGW(TAG, "recovering I2C bus after %d failures", failures);
                bme280_recover();
                ready = false;
            }
        }
    }
}

// The BME280's rated operating range (datasheet section 1). Anything outside it is a
// bad reading, and it would also overflow the int16 and uint16 fields in the CAN frame.
static bool reading_in_range(const BmeReading &r)
{
    return r.temp_c100 >= -4000 && r.temp_c100 <= 8500 &&
           r.hum_c100 <= 10000 &&
           r.press_pa >= 30000 && r.press_pa <= 110000;
}

static void processing_task(void *arg)
{
    Sample s;
    while (true) {
        // Block with no timeout: this task uses no CPU until a sample arrives.
        xQueueReceive(s_sample_queue, &s, portMAX_DELAY);

        if (!reading_in_range(s.reading)) {
            ESP_LOGW(TAG, "seq %" PRIu32 " out of range, dropped: T=%" PRId32 " P=%" PRIu32 " H=%" PRIu32,
                     s.seq, s.reading.temp_c100, s.reading.press_pa, s.reading.hum_c100);
            continue;
        }

        EnvReading env = {s.reading.temp_c100, s.reading.press_pa, s.reading.hum_c100};
        Outgoing out = {s.seq, s.on_demand, can_pack_reading(env)};
        send_latest(s_frame_queue, out, "frame");
    }
}

static void send_heartbeat()
{
    uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    bool sensor_ok = s_sensor_ok;
    CanFrame hb = can_pack_heartbeat(uptime_s, sensor_ok);
    esp_err_t err = can_bus_send(hb.id, hb.data, hb.len);
    ESP_LOGI(TAG, "HB id=0x%03" PRIX32 " uptime=%" PRIu32 "s sensor=%s can=%s",
             hb.id, uptime_s, sensor_ok ? "ok" : "down", esp_err_to_name(err));

    // Every 10 heartbeats, report the CAN controller's health: state and error counters.
    if (uptime_s % 10 == 0) {
        can_bus_log_status();
    }
}

static void send_reading(const Outgoing &out)
{
    esp_err_t can_err = can_bus_send(out.frame.id, out.frame.data, out.frame.len);
    ble_env_publish(out.frame.data);  // BLE carries the same 8 bytes as the CAN frame

    // Log the frame, and decode it back to prove the packing round trips.
    const uint8_t *d = out.frame.data;
    EnvReading r = can_unpack_reading(out.frame);
    ESP_LOGI(TAG, "TX id=0x%03" PRIX32 " seq=%" PRIu32 "%s [%02X %02X %02X %02X %02X %02X %02X %02X]"
                  "  T=%" PRId32 ".%02" PRId32 " C  H=%" PRIu32 ".%02" PRIu32 " %%  P=%" PRIu32 " Pa  can=%s",
             out.frame.id, out.seq, out.on_demand ? " (button)" : "",
             d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7],
             r.temp_c100 / 100, r.temp_c100 % 100,
             r.hum_c100 / 100, r.hum_c100 % 100,
             r.press_pa, esp_err_to_name(can_err));
}

static void comms_task(void *arg)
{
    TickType_t next_heartbeat = xTaskGetTickCount() + HEARTBEAT_PERIOD;
    while (true) {
        // Wait for a reading, but no longer than until the next heartbeat is due, so
        // heartbeats keep going even when the sensor produces nothing.
        int32_t remaining = (int32_t)(next_heartbeat - xTaskGetTickCount());
        TickType_t wait = remaining > 0 ? (TickType_t)remaining : 0;

        Outgoing out;
        if (xQueueReceive(s_frame_queue, &out, wait) == pdTRUE) {
            send_reading(out);
        }

        if ((int32_t)(next_heartbeat - xTaskGetTickCount()) <= 0) {
            send_heartbeat();
            next_heartbeat += HEARTBEAT_PERIOD;
            if ((int32_t)(next_heartbeat - xTaskGetTickCount()) <= 0) {
                next_heartbeat = xTaskGetTickCount() + HEARTBEAT_PERIOD;  // fell behind, resync
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Startup
// ---------------------------------------------------------------------------

static void start_task(TaskFunction_t fn, const char *name, UBaseType_t prio, TaskHandle_t *handle)
{
    if (xTaskCreate(fn, name, STACK_BYTES, nullptr, prio, handle) != pdPASS) {
        ESP_LOGE(TAG, "failed to start task %s", name);
        abort();
    }
}

extern "C" void app_main(void)
{
    ESP_ERROR_CHECK(ble_env_init());
    ESP_ERROR_CHECK(can_bus_init(CAN_SELF_TEST));

    s_sample_queue = xQueueCreate(QUEUE_LENGTH, sizeof(Sample));
    s_frame_queue = xQueueCreate(QUEUE_LENGTH, sizeof(Outgoing));
    if (s_sample_queue == nullptr || s_frame_queue == nullptr) {
        ESP_LOGE(TAG, "failed to create queues");
        abort();
    }

    // Start consumers first, so they are already waiting when the first data arrives.
    start_task(comms_task, "comms", PRIO_COMMS, nullptr);
    start_task(processing_task, "processing", PRIO_PROCESSING, nullptr);
    start_task(sensor_task, "sensor", PRIO_SENSOR, &s_sensor_task);

    // The ISR notifies the sensor task, so enable it only once that task exists.
    button_init();

    // app_main returns here. Its task is deleted, and the three tasks keep running.
}
