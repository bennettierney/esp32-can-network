#pragma once

// CAN bus (TWAI) transmitter for node A.
// Wiring: GPIO 4 to CAN Pal TX, GPIO 5 to CAN Pal RX (TX to TX, RX to RX). 500 kbps.

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Start the TWAI controller. With self_test true, frames do not need an acknowledgment
// from another node, so one board can check its own transceiver wiring.
esp_err_t can_bus_init(bool self_test);

// Send one classic CAN frame (standard 11 bit ID, up to 8 bytes) and wait until it has
// gone out on the bus or failed. Recovers the controller if it went bus off.
esp_err_t can_bus_send(uint32_t id, const uint8_t *data, uint8_t len);

// Log the controller state, error counters and how many frames succeeded or failed.
void can_bus_log_status(void);

#ifdef __cplusplus
}
#endif
