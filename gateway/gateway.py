"""Laptop gateway: connects to node B over BLE and receives readings and status.

Usage:
    .venv/bin/python gateway.py                 # run until Ctrl+C
    .venv/bin/python gateway.py --duration 20   # run for 20 seconds, then exit

Reconnects automatically if node B goes away.
"""

import argparse
import asyncio
import logging

from bleak import BleakClient, BleakScanner

from protocol import (
    DEVICE_NAME,
    READING_UUID,
    STATUS_UUID,
    decode_reading,
    decode_status,
)

log = logging.getLogger("gateway")

SCAN_TIMEOUT_S = 15
RECONNECT_DELAY_S = 3


def on_reading(_sender, data: bytearray) -> None:
    try:
        r = decode_reading(bytes(data))
    except ValueError as e:
        log.warning("bad reading: %s", e)
        return
    log.info("reading  T=%.2f C  H=%.2f %%  P=%d Pa", r.temperature_c, r.humidity_pct, r.pressure_pa)


def on_status(_sender, data: bytearray) -> None:
    try:
        s = decode_status(bytes(data))
    except ValueError as e:
        log.warning("bad status: %s", e)
        return
    log.info(
        "status   node A %s, sensor %s, uptime %d s, CAN errors %d",
        "online" if s.node_a_online else "OFFLINE",
        "ok" if s.sensor_ok else "DOWN",
        s.node_a_uptime_s,
        s.can_errors,
    )


async def run_once() -> None:
    """Find node B, connect, subscribe, and return when the connection drops."""
    log.info("scanning for %s", DEVICE_NAME)
    device = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=SCAN_TIMEOUT_S)
    if device is None:
        log.warning("%s not found", DEVICE_NAME)
        return

    disconnected = asyncio.Event()
    async with BleakClient(device, disconnected_callback=lambda _c: disconnected.set()) as client:
        log.info("connected to %s (%s)", DEVICE_NAME, device.address)
        await client.start_notify(READING_UUID, on_reading)
        await client.start_notify(STATUS_UUID, on_status)
        await disconnected.wait()
        log.warning("disconnected")


async def run_forever() -> None:
    while True:
        try:
            await run_once()
        except Exception as e:  # BLE errors are common (out of range, busy); keep going
            log.warning("BLE error: %s", e)
        await asyncio.sleep(RECONNECT_DELAY_S)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--duration", type=float, help="stop after this many seconds")
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")

    async def run() -> None:
        if args.duration:
            try:
                await asyncio.wait_for(run_forever(), timeout=args.duration)
            except asyncio.TimeoutError:
                log.info("done")
        else:
            await run_forever()

    try:
        asyncio.run(run())
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
