"""Laptop gateway: receives readings and status from node B over BLE and uploads them
to the backend.

Usage:
    .venv/bin/python gateway.py                       # run until Ctrl+C
    .venv/bin/python gateway.py --duration 20         # run for 20 seconds, then exit
    .venv/bin/python gateway.py --no-backend          # BLE only, just log

Reconnects automatically if node B goes away. If the backend is down, readings are
buffered (up to an hour's worth) and uploaded when it comes back.
"""

import argparse
import asyncio
import logging
from collections import deque
from dataclasses import asdict
from datetime import datetime, timezone

import httpx
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
BUFFER_LIMIT = 3600 * 2  # one reading and one status per second, so about an hour
UPLOAD_RETRY_S = 2


class Uploader:
    """Sends items to the backend in order, buffering while the backend is unreachable."""

    def __init__(self, base_url: str) -> None:
        self.base_url = base_url
        # A full buffer drops the oldest item: recent data matters most, and memory stays bounded.
        self.buffer: deque[tuple[str, dict]] = deque(maxlen=BUFFER_LIMIT)
        self.pending = asyncio.Event()

    def submit(self, path: str, payload: dict) -> None:
        self.buffer.append((path, payload))
        self.pending.set()

    async def run(self) -> None:
        async with httpx.AsyncClient(base_url=self.base_url, timeout=5) as client:
            while True:
                await self.pending.wait()
                self.pending.clear()
                while self.buffer:
                    path, payload = self.buffer[0]
                    try:
                        response = await client.post(path, json=payload)
                    except httpx.HTTPError as e:
                        log.warning("backend unreachable (%s), %d items buffered", e.__class__.__name__,
                                    len(self.buffer))
                        await asyncio.sleep(UPLOAD_RETRY_S)
                        continue
                    if response.status_code >= 500:
                        log.warning("backend error %d, retrying", response.status_code)
                        await asyncio.sleep(UPLOAD_RETRY_S)
                        continue
                    if response.status_code >= 400:
                        # The backend rejected the data itself; retrying would never succeed.
                        log.warning("backend rejected %s: %s", path, response.text)
                    self.buffer.popleft()


def now_utc() -> str:
    # Node A has no clock, so the gateway timestamps each value when it arrives.
    return datetime.now(timezone.utc).isoformat()


def make_handlers(uploader: Uploader | None):
    def on_reading(_sender, data: bytearray) -> None:
        try:
            r = decode_reading(bytes(data))
        except ValueError as e:
            log.warning("bad reading: %s", e)
            return
        log.info("reading  T=%.2f C  H=%.2f %%  P=%d Pa", r.temperature_c, r.humidity_pct, r.pressure_pa)
        if uploader:
            uploader.submit("/api/readings", {"node": "node_a", "recorded_at": now_utc(), **asdict(r)})

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
        if uploader:
            uploader.submit("/api/status", {
                "node": "node_a",
                "recorded_at": now_utc(),
                "online": s.node_a_online,
                "sensor_ok": s.sensor_ok,
                "uptime_s": s.node_a_uptime_s,
                "can_errors": s.can_errors,
            })

    return on_reading, on_status


async def run_once(on_reading, on_status) -> None:
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


async def run_ble_forever(on_reading, on_status) -> None:
    while True:
        try:
            await run_once(on_reading, on_status)
        except Exception as e:  # BLE errors are common (out of range, busy); keep going
            log.warning("BLE error: %s", e)
        await asyncio.sleep(RECONNECT_DELAY_S)


async def run(args: argparse.Namespace) -> None:
    uploader = None if args.no_backend else Uploader(args.backend)
    on_reading, on_status = make_handlers(uploader)

    tasks = [run_ble_forever(on_reading, on_status)]
    if uploader:
        log.info("uploading to %s", args.backend)
        tasks.append(uploader.run())
    work = asyncio.gather(*tasks)

    if args.duration:
        try:
            await asyncio.wait_for(work, timeout=args.duration)
        except asyncio.TimeoutError:
            log.info("done")
    else:
        await work


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--backend", default="http://localhost:8000", help="backend base URL")
    parser.add_argument("--no-backend", action="store_true", help="only log, do not upload")
    parser.add_argument("--duration", type=float, help="stop after this many seconds")
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    try:
        asyncio.run(run(args))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
