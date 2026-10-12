#include <inttypes.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bme280.h"

static const char *TAG = "node_a";

// After this many failed reads in a row, recover the bus and re-initialize the sensor.
static constexpr int MAX_CONSECUTIVE_FAILURES = 3;

extern "C" void app_main(void)
{
    ESP_ERROR_CHECK(bme280_bus_init());
    bme280_scan_bus();

    bool ready = false;  // true once bme280_init has succeeded
    int failures = 0;    // consecutive failed reads

    while (true) {
        // Bring the sensor up, at boot or after a recovery. Keep retrying instead of
        // giving up, so a loose wire or a glitch at power on is not fatal.
        if (!ready) {
            esp_err_t err = bme280_init();
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "sensor ready");
                ready = true;
                failures = 0;
            } else {
                ESP_LOGW(TAG, "sensor init failed: %s, recovering I2C bus", esp_err_to_name(err));
                bme280_recover();
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
        }

        BmeReading r;
        esp_err_t err = bme280_read(&r);
        if (err == ESP_OK) {
            failures = 0;
            ESP_LOGI(TAG, "T=%" PRId32 ".%02" PRId32 " C  P=%" PRIu32 " Pa  H=%" PRIu32 ".%02" PRIu32 " %%",
                     r.temp_c100 / 100, r.temp_c100 % 100,
                     r.press_pa,
                     r.hum_c100 / 100, r.hum_c100 % 100);
        } else if (err == ESP_ERR_NOT_FINISHED) {
            // First measurement after configure is not ready yet. Not a bus problem.
            ESP_LOGD(TAG, "measurement not ready yet");
        } else {
            failures++;
            ESP_LOGE(TAG, "read failed (%d in a row): %s", failures, esp_err_to_name(err));
            if (failures >= MAX_CONSECUTIVE_FAILURES) {
                // The bus or the I2C controller is stuck, for example after a glitch on
                // SDA or SCL mid transfer. Rebuild the bus, then re-initialize the sensor.
                ESP_LOGW(TAG, "recovering I2C bus after %d failures", failures);
                bme280_recover();
                ready = false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
