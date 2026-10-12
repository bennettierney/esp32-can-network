"""Decoding of the BLE values published by node B.

Byte layouts match firmware/components/can_protocol/include/can_frame.h and
firmware/node_b/main/ble_env.h. Everything is little endian.
"""

import struct
from dataclasses import dataclass

DEVICE_NAME = "esp32-env"
SERVICE_UUID = "4a5c0001-7f3e-4b8e-9d2a-1c6f0e8b3a55"
READING_UUID = "4a5c0002-7f3e-4b8e-9d2a-1c6f0e8b3a55"
STATUS_UUID = "4a5c0003-7f3e-4b8e-9d2a-1c6f0e8b3a55"

# reading: temperature int16 (0.01 C), humidity uint16 (0.01 %RH), pressure uint32 (Pa)
_READING = struct.Struct("<hHI")
# status: flags uint8, node A uptime uint32 (s), CAN bus errors seen by node B uint32
_STATUS = struct.Struct("<BII")

FLAG_NODE_A_ONLINE = 0x01
FLAG_SENSOR_OK = 0x02


@dataclass(frozen=True)
class Reading:
    temperature_c: float
    humidity_pct: float
    pressure_pa: int


@dataclass(frozen=True)
class Status:
    node_a_online: bool
    sensor_ok: bool
    node_a_uptime_s: int
    can_errors: int


def decode_reading(data: bytes) -> Reading:
    if len(data) != _READING.size:
        raise ValueError(f"reading must be {_READING.size} bytes, got {len(data)}")
    temp_c100, hum_c100, press_pa = _READING.unpack(data)
    return Reading(temp_c100 / 100, hum_c100 / 100, press_pa)


def decode_status(data: bytes) -> Status:
    if len(data) != _STATUS.size:
        raise ValueError(f"status must be {_STATUS.size} bytes, got {len(data)}")
    flags, uptime_s, can_errors = _STATUS.unpack(data)
    return Status(
        node_a_online=bool(flags & FLAG_NODE_A_ONLINE),
        sensor_ok=bool(flags & FLAG_SENSOR_OK),
        node_a_uptime_s=uptime_s,
        can_errors=can_errors,
    )
