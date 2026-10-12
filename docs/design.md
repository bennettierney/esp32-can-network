# Design notes and tradeoffs

Why the system is built the way it is, and what went wrong on the way. The hardware is
premade modules on breadboards (ESP32-S3 dev boards, an Adafruit BME280 breakout, two
Adafruit CAN Pal transceivers); the work here is the firmware, the protocol, and the
software around it.

## System shape: a tiny vehicle network

A car is a network of small computers (ECUs) on a CAN bus, with a gateway that bridges
the internal bus to the outside world. This project copies that split:

- **Node A, the sensor ECU**, reads the BME280 and broadcasts readings and a heartbeat on
  CAN. It has no radio.
- **Node B, the gateway ECU**, listens on CAN, tracks node A's health, and republishes
  everything over BLE.
- **The laptop** is the cloud side: a Python gateway uploads to a FastAPI service backed
  by Postgres, and a React dashboard shows it live.

One board could do all of this alone. The second node exists because a real bus, with
acknowledgments, arbitration, termination and error counters, is the point.

## Firmware

### CAN with the built in controller, not an MCP2515

The ESP32-S3 has a CAN controller on chip (TWAI in ESP-IDF), so each node only needs a
transceiver (TJA1051 on the CAN Pal) to convert 3.3 V logic to the differential bus. An
SPI CAN controller like the MCP2515 would add a chip, an SPI driver and interrupt
handling for no benefit.

### BME280 driver with raw register reads

The driver talks to the sensor directly over I2C instead of using a vendor library:
chip ID check, soft reset with a wait on the `im_update` status bit, a burst read of the
33 calibration bytes, oversampling and mode configuration, and a burst read of the 8 data
bytes.

- **Integer compensation.** The datasheet's 32 bit integer formulas turn raw counts into
  0.01 C, Pa and 0.01 %RH. Integers keep results identical on every build and match the
  units on the wire. They were checked on the host against the datasheet's floating
  point formulas using this chip's real calibration constants: within 0.004 C, 0.006 %RH
  and 3.5 Pa (the 32 bit pressure formula's known precision limit).
- **One burst read for all measurements.** The chip freezes its output registers during
  a burst read, so temperature, pressure and humidity always come from the same
  measurement.
- **Skipped value check.** Before the first measurement completes, the data registers
  hold `0x80000`. The driver reports that as "not finished" instead of compensating it
  into a nonsense pressure (it produced 71377 Pa before the check existed).

### I2C bus recovery

A glitch on SDA or SCL mid transfer (a probe being attached, a jumper being bumped) can
leave the I2C controller stuck, with every later transfer failing. After three failed
reads in a row, node A tears the bus down and rebuilds it, then re-initializes the
sensor. `i2c_master_bus_reset()` alone was not enough: it clears the bus lines but the
driver stays in its error state. Verified by shorting SDA to ground for 5 seconds:
readings resumed on their own with no reboot.

### FreeRTOS structure

Node A runs three tasks connected by two queues:

```
sensor_task ──▶ [queue] ──▶ processing_task ──▶ [queue] ──▶ comms_task
     ▲
BOOT button interrupt
```

- **Each task owns one job.** Only the sensor task touches I2C; only the comms task
  touches CAN. Tasks share data by copying messages through queues, never through shared
  variables.
- **Drift free schedule.** The sensor task computes each wake time from the previous
  scheduled time, not from "now", so slow reads never shift the 1 second rhythm.
- **Full queue drops the oldest item.** For telemetry a fresh reading is worth more than
  a stale one, and the producer must never block on a slow consumer. Queues hold 8 items.
- **Priorities:** sensor 5, processing 4, comms 3. Sampling stays on time; downstream
  tasks may lag briefly and the queues absorb it.
- **The interrupt does almost nothing.** The BOOT button ISR debounces (200 ms), then
  sends a task notification to the sensor task and yields. No logging, no I2C, no
  blocking in interrupt context.

### CAN protocol

Standard 11 bit IDs at 500 kbps. Payloads are little endian and packed with shifts, not
by copying structs, so the byte order does not depend on the compiler. Both nodes build
from one shared header (`firmware/components/can_protocol/include/can_frame.h`), so
packing and unpacking cannot drift apart.

| ID | Frame | Payload (8 bytes) |
|---|---|---|
| `0x100` | environment reading | temperature int16 (0.01 C), humidity uint16 (0.01 %RH), pressure uint32 (Pa) |
| `0x701` | node A heartbeat, every 1 s | uptime uint32 (s), sensor_ok uint8, 3 reserved bytes |

- **Fixed point integers, not floats,** keep every value in 8 bytes with exact, portable
  encoding.
- **IDs double as priorities.** On a CAN bus the lower ID wins arbitration, so readings
  (`0x100`) beat heartbeats (`0x701`).
- **Heartbeat on `0x700` + node number** follows the CANopen convention. It lets node B
  tell "node A is offline" (heartbeats stop; flagged after 3 s) apart from "node A is up
  but its sensor failed" (heartbeats continue with `sensor_ok = 0`). It also made CAN
  testable while the sensor was unreliable.
- **Readings are range checked before packing** against the BME280's rated limits. That
  rejects bad data and guarantees temperature fits the int16 field.

### Reporting CAN failures honestly

`twai_node_transmit()` and `twai_node_transmit_wait_all_done()` return `ESP_OK` even when
the frame then fails on the bus; the failure only arrives through the `on_tx_done`
callback. The `can_bus` module compares the failure count before and after each send so
the caller sees the real outcome. Every 10 seconds it also logs the controller state and
error counters, and it recovers the controller if it goes bus off.

### BLE on the gateway

Node B runs a NimBLE GATT server (NimBLE is smaller than Bluedroid and BLE only is all
that is needed). One custom service with two read and notify characteristics:

| UUID | Value |
|---|---|
| `4a5c0002-7f3e-4b8e-9d2a-1c6f0e8b3a55` | reading: the same 8 bytes as the CAN payload, passed through unchanged |
| `4a5c0003-7f3e-4b8e-9d2a-1c6f0e8b3a55` | status, 9 bytes: flags (bit 0 node A online, bit 1 sensor ok), node A uptime uint32, CAN errors uint32 |

Keeping the BLE reading byte for byte identical to the CAN payload means every consumer
decodes one format.

## Laptop side

- **Gateway (`gateway/`).** Python with bleak. It timestamps each value on arrival,
  because node A has no real time clock. Uploads are buffered (about an hour) while the
  backend is unreachable; values the backend rejects as invalid are dropped rather than
  retried forever. The byte layouts live in `protocol.py` with unit tests that use real
  bytes from the boards' logs.
- **Backend (`backend/`).** FastAPI and Postgres 16 (via Docker Compose). Readings go in
  one table indexed by node and time; node status is one row per node, overwritten on
  each update. A WebSocket pushes each new value to connected dashboards. Request
  validation uses the same physical limits as the firmware.
- **Dashboard (`dashboard/`).** React with Recharts. History loads over HTTP and then
  stays live over the WebSocket. One chart per measure, never two y axes on one chart.
  Health badges use an icon and a word, never color alone. Data counts as stale after 5
  seconds without a reading, which catches failures the nodes cannot report themselves
  (a stopped gateway or node B).

## What went wrong, and what it taught

- **Missing termination.** Node A's CAN frames all failed in self test mode even though
  a slow static test of the wiring passed. With the termination switch off and no other
  node, the bus could not return to recessive within a 2 µs bit at 500 kbps. Switching on
  the 120 Ω terminator fixed it.
- **A short in the screw terminals.** Bare jumper pins touching in a screw terminal
  shorted CANH to CANL, so neither transceiver could drive a dominant bit. Node A saw bit
  errors and node B saw a silent bus. Moving the bus to the transceivers' header pins
  fixed it.
- **A logic analyzer that loaded the bus.** A low cost clone analyzer held I2C low after
  captures and wedged the bus. Diagnosing that is what led to the I2C recovery code. A
  clean decoded capture is still outstanding.
- **Loose breadboard wiring.** Most failures were physical: jumpers worked loose when the
  board was touched. Two throwaway test firmwares helped: one reads which GPIOs are pulled
  high or low by something external, the other drives the CAN TX pin and checks that the
  transceiver echoes it on RX. Both turned guesses into measurements.
- **A misleading success code.** The CAN driver reporting `ESP_OK` for failed frames hid
  the termination problem at first, until the error counters were logged.
