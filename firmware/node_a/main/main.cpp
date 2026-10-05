/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "esp_log.h"
#include "led_strip.h"

static const char *TAG = "node_a";

static constexpr int LED_GPIO = 48;  // DevKitC-1 v1.0 is 48, v1.1 is 38
static constexpr int LED_MAX = 64;   // cap brightness, 255 is painful

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Hello world! This is Bennet's Micro Processor");

    /* Print chip information */
    esp_chip_info_t chip_info;
    uint32_t flash_size;
    esp_chip_info(&chip_info);
    ESP_LOGI(TAG, "This is %s chip with %d CPU core(s), %s%s%s%s",
             CONFIG_IDF_TARGET,
             chip_info.cores,
             (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi/" : "",
             (chip_info.features & CHIP_FEATURE_BT) ? "BT" : "",
             (chip_info.features & CHIP_FEATURE_BLE) ? "BLE" : "",
             (chip_info.features & CHIP_FEATURE_IEEE802154) ? ", 802.15.4 (Zigbee/Thread)" : "");

    unsigned major_rev = chip_info.revision / 100;
    unsigned minor_rev = chip_info.revision % 100;
    ESP_LOGI(TAG, "silicon revision v%d.%d", major_rev, minor_rev);
    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        ESP_LOGE(TAG, "Get flash size failed");
        return;
    }

    ESP_LOGI(TAG, "%" PRIu32 "MB %s flash", flash_size / (uint32_t)(1024 * 1024),
             (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");

    ESP_LOGI(TAG, "Minimum free heap size: %" PRIu32 " bytes", esp_get_minimum_free_heap_size());

    led_strip_handle_t strip;

    led_strip_config_t strip_cfg = {};
    strip_cfg.strip_gpio_num = LED_GPIO;
    strip_cfg.max_leds = 1;
    strip_cfg.led_model = LED_MODEL_WS2812;
    strip_cfg.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;

    led_strip_rmt_config_t rmt_cfg = {};
    rmt_cfg.resolution_hz = 10 * 1000 * 1000;  // 10 MHz

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &strip));

    // Different start values and step sizes so the channels drift apart
    int r = 0, g = 20, b = 40;

    while (true) {
        led_strip_set_pixel(strip, 0, r, g, b);
        led_strip_refresh(strip);  // nothing lights until this
        ESP_LOGI(TAG, "led r=%d g=%d b=%d", r, g, b);

        r = (r + 3) % LED_MAX;
        g = (g + 5) % LED_MAX;
        b = (b + 7) % LED_MAX;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
