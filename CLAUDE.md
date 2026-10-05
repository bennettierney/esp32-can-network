# Project handoff: ESP32 two node CAN sensor network

## Who you are working with

Bennet, a full stack software engineer (Python, FastAPI, React, TypeScript, PostgreSQL, AWS). College coursework was all C++, so syntax is familiar but embedded practice is new. Based in Raleigh, NC.

Communication preferences: terse, direct, honest. No hedging, no overly formal language. Never use em dashes or hyphens as dashes in any writing (comments, docs, commit messages, replies). Use commas, periods, or rewrite the sentence.

## Why this project exists

Portfolio project to move toward embedded firmware and autonomous vehicle (AV) work. It must be something he can explain in an interview, so he needs to understand and write the core pieces himself.

How to work with him:

* He is learning. Explain the why briefly, then let him write the driver code. Review, suggest, and fix, but do not hand over finished drivers unless he asks.
* The BME280 driver must use raw register reads. No Arduino or vendor sensor libraries for that part.
* Prefer ESP-IDF over the Arduino framework.
* Language is C++ on the device, Python for the gateway and backend, TypeScript and React for the dashboard.
* Commit small and often. The git history is part of the proof of work.
* Do not claim hardware design or PCB experience in any write-up. He wires premade modules.

## What we are building

A two node sensor network that mimics a tiny vehicle ECU network, ending in a live dashboard.

Data path:

1. Node A (sensor node): ESP32-S3 reads temperature, humidity, and pressure from a BME280 over I2C. FreeRTOS tasks handle sampling and packaging. It sends each reading as a CAN frame.
2. Node B (gateway node): a second ESP32-S3 receives the CAN frames and re-broadcasts them over BLE as a GATT service (notify).
3. Laptop: a Python script (bleak) connects over BLE and POSTs readings to a FastAPI service.
4. Backend and dashboard: FastAPI stores readings in PostgreSQL. A React page charts them live.

Target: desk setup. Mounting Node A on a bike for ride logging is a possible later extension, not in scope now.

## Hardware

Status as of 2026-10-05: nothing is built or wired yet. Parts are arriving.

* 2x ESP32-S3-DevKitC-1 clones, N16R8 (16MB flash, 8MB octal PSRAM), AITRIP 3 pack from Amazon. Two USB-C ports per board. Use the one labeled UART/COM for flashing. Due Oct 4.
* 1x Adafruit BME280 breakout (PID 2652, I2C, STEMMA QT). Header must be soldered. Expected Oct 6.
* 2x Adafruit CAN Pal (PID 5708, TJA1051T/3). Expected Oct 6.
   * Runs from 3.3V power and logic. It makes its own 5V with a charge pump, so no separate 5V wiring.
   * Has an onboard 120 ohm termination (2x 60 ohm in series) with a switch. Turn it ON for both boards on a two node bus.
   * The 3.5mm terminal block (CANH, CANL, GND) is pre-soldered. The logic header is NOT, and must be soldered.
   * Labeled pins: VCC, RX, TX, GND, CANH, CANL. Check the Adafruit CAN Pal guide for the exact RX/TX direction before wiring.
* 2x full size 830 point breadboards, 1x pack of 40 male/male jumper wires (6 inch). One breadboard per node.
* BEST 102C soldering iron, iron stand, 60/40 rosin core solder (0.3mm). Set the iron around 320 to 350C.
* HiLetgo 24MHz 8 channel USB logic analyzer (recommended, confirm it was ordered). Used with the free PulseView software. Expected around Oct 8. Needed from week 2.
* Need a USB-C data cable (not charge only). Confirm he has one.

Why no MCP2515: the ESP32-S3 has a built in CAN controller (called TWAI in ESP-IDF), so only a transceiver is needed. Do not add an MCP2515 or an SPI CAN driver.

## Pin notes (verify against the Espressif docs for the exact module)

Not final. Confirm against the ESP32-S3-DevKitC-1 and ESP32-S3-WROOM-1 documentation before wiring.

* Avoid GPIO 35, 36, 37: used by the octal PSRAM on N16R8 modules.
* Avoid GPIO 26 to 32: connected to flash and PSRAM on the module.
* Strapping pins to avoid for general use: GPIO 0, 3, 45, 46.
* GPIO 19 and 20 are the native USB D- and D+.
* Candidate assignments to check: I2C SDA on GPIO 8, SCL on GPIO 9. CAN TX on GPIO 4, RX on GPIO 5.
* BME280 I2C address is 0x76 or 0x77. Reading chip ID register 0xD0 should return 0x60 for a real BME280. 0x58 means it is a BMP280, which has no humidity sensor.

## Proposed CAN message format (a starting point, he should own the final design)

* Standard 11 bit ID, 500 kbps bitrate.
* ID 0x100, 8 byte payload, little endian: temperature int16 in 0.01 C, humidity uint16 in 0.01 percent, pressure uint32 in Pa.
* Later extras to consider: a heartbeat frame, a sequence counter, an error frame.

## Plan: 12 weeks, about 6 to 8 hours a week

1. Week 1: install ESP-IDF, set target `esp32s3`, run hello_world, blink an LED, add serial logging with ESP_LOGI. Done when he can build, flash, and monitor from the command line.
2. Weeks 2 to 3: BME280 over I2C using raw register reads. Verify bus traffic on the logic analyzer in PulseView. Done when readings match a sanity check and the I2C decode matches the code.
3. Weeks 4 to 5: FreeRTOS with sampling, processing, and comms tasks connected by queues. Handle one interrupt properly.
4. Weeks 6 to 7: BLE GATT service exposing the readings. Test with the nRF Connect phone app.
5. Weeks 8 to 9: CAN between two nodes using TWAI and the CAN Pals. Define and document the frame format. Both CAN Pal termination switches ON.
6. Weeks 10 to 11: Python bleak gateway on the laptop, FastAPI service, PostgreSQL storage, React live chart. Treat it like a small fleet telemetry pipeline.
7. Week 12: polish. README with wiring diagram, logic analyzer captures, and a short design tradeoffs write up. Link from his portfolio.

He plans to start applying around week 4 and mention the project in progress.

## Repo layout

```
/firmware/node_a      ESP-IDF project, sensor and CAN transmit
/firmware/node_b      ESP-IDF project, CAN receive and BLE
/gateway              Python bleak script
/backend              FastAPI and Postgres schema
/dashboard            React app
/docs                 wiring diagram, captures, design notes
```

## Known gotchas

* Charge only USB cables will not flash the board. If no serial port appears, swap the cable first.
* These are clone boards, so quality varies. A spare board is available (3 pack).
* Some cheap BME280 listings are really BMP280. The Adafruit part is genuine, but check the chip ID anyway.
* Loose headers on the BME280 and CAN Pal need soldering before breadboard use. Hold the header in a breadboard while soldering so it stays straight.

## Open items

* Confirm the Adafruit order, logic analyzer order, and a data capable USB-C cable.
* Finalize the pin map after checking the Espressif docs.
* Decide CAN bitrate and final frame layout.

## Context for the larger goal

Target roles are embedded firmware and AV simulation, test infrastructure, and data tooling. The telemetry pipeline in weeks 10 to 11 is intentionally shaped like the data and simulation tooling those AV roles describe, so keep it clean and well documented.
