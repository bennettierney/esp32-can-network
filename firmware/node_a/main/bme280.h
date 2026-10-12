#pragma once

// BME280 driver over I2C, using raw register reads.
// Wiring: SDA on GPIO 8, SCL on GPIO 9, sensor at address 0x77.

#include <stdint.h>
#include "esp_err.h"

// One compensated measurement, in the units used by the CAN frame.
struct BmeReading {
    int32_t  temp_c100;  // temperature in 0.01 C, 2195 means 21.95 C
    uint32_t press_pa;   // pressure in Pa
    uint32_t hum_c100;   // relative humidity in 0.01 %, 5902 means 59.02 %
};

// Create the I2C bus and register the sensor on it. Call once at startup.
esp_err_t bme280_bus_init();

// Log every address that answers on the bus. For bring-up and debugging.
void bme280_scan_bus();

// Verify the chip ID, soft reset, load calibration, and start measuring.
// Call after bme280_bus_init, and again after bme280_recover.
esp_err_t bme280_init();

// Read one measurement and compensate it into real units.
// Returns ESP_ERR_NOT_FINISHED if the sensor has not produced its first measurement yet.
esp_err_t bme280_read(BmeReading *out);

// Tear down and rebuild the I2C bus after repeated failures.
// The sensor must be re-initialized with bme280_init afterwards.
esp_err_t bme280_recover();
