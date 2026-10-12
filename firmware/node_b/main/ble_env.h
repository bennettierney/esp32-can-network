#pragma once

// BLE GATT server that publishes environment readings.
//
// Runs on node B, the gateway. Advertises as "esp32-env". One custom service with two
// characteristics, both read + notify, little endian:
//   reading   8 bytes, the same layout as the CAN payload from node A (can_frame.h):
//             temperature int16 0.01 C, humidity uint16 0.01 %RH, pressure uint32 Pa.
//   status    9 bytes: flags uint8 (bit 0 node A online, bit 1 node A sensor ok),
//             node A uptime uint32 seconds, CAN bus errors seen by node B uint32.
//
// Written in C because NimBLE's service tables use C99 compound literals.

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_ENV_PAYLOAD_LEN 8
#define BLE_ENV_STATUS_LEN 9

// Start NVS, the NimBLE stack, the GATT service, and advertising. Call once at boot.
esp_err_t ble_env_init(void);

// Store the latest value and notify every subscribed client. Safe to call from any task.
void ble_env_publish_reading(const uint8_t payload[BLE_ENV_PAYLOAD_LEN]);
void ble_env_publish_status(const uint8_t payload[BLE_ENV_STATUS_LEN]);

#ifdef __cplusplus
}
#endif
