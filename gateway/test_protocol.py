"""Decoder tests using real bytes captured from node A's logs."""

import pytest

from protocol import decode_reading, decode_status


def test_reading_from_node_a_log():
    # node_a: TX id=0x100 [5E 09 93 1A 73 87 01 00]  T=23.98 C  H=68.03 %  P=100211 Pa
    r = decode_reading(bytes([0x5E, 0x09, 0x93, 0x1A, 0x73, 0x87, 0x01, 0x00]))
    assert r.temperature_c == pytest.approx(23.98)
    assert r.humidity_pct == pytest.approx(68.03)
    assert r.pressure_pa == 100211


def test_negative_temperature():
    # -5.25 C is -525, two's complement 0xFDF3, sent low byte first
    r = decode_reading(bytes([0xF3, 0xFD, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00]))
    assert r.temperature_c == pytest.approx(-5.25)


def test_status_flags_and_counters():
    # flags 0x03 (online, sensor ok), uptime 360 s (0x168), 7 CAN errors
    s = decode_status(bytes([0x03, 0x68, 0x01, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00]))
    assert s.node_a_online and s.sensor_ok
    assert s.node_a_uptime_s == 360
    assert s.can_errors == 7


def test_status_node_a_offline():
    s = decode_status(bytes([0x00] * 9))
    assert not s.node_a_online and not s.sensor_ok


def test_wrong_length_rejected():
    with pytest.raises(ValueError):
        decode_reading(b"\x00" * 7)
    with pytest.raises(ValueError):
        decode_status(b"\x00" * 8)
