#pragma once

// BLE GATT server that publishes environment readings.
//
// Advertises as "esp32-env". One custom service with one characteristic:
//   reading   read + notify, 8 bytes, the same layout as the CAN payload (can_frame.h):
//             temperature int16 0.01 C, humidity uint16 0.01 %RH, pressure uint32 Pa,
//             little endian.
//
// Written in C because NimBLE's service tables use C99 compound literals.

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_ENV_PAYLOAD_LEN 8

// Start NVS, the NimBLE stack, the GATT service, and advertising. Call once at boot.
esp_err_t ble_env_init(void);

// Store the latest payload and notify every subscribed client. Safe to call from any task.
void ble_env_publish(const uint8_t payload[BLE_ENV_PAYLOAD_LEN]);

#ifdef __cplusplus
}
#endif
