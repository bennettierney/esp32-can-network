#pragma once

// CAN frame layout for one environment reading.
//
// Standard 11 bit ID, 8 byte payload, little endian (low byte first):
//   bytes 0 to 1   temperature   int16    0.01 C
//   bytes 2 to 3   humidity      uint16   0.01 %RH
//   bytes 4 to 7   pressure      uint32   Pa
//
// Packing is done by hand with shifts, not by copying a struct, so the byte order
// on the wire does not depend on the compiler or the CPU.

#include <stdint.h>
#include "bme280.h"

static constexpr uint32_t CAN_ID_ENV_READING = 0x100;

// Heartbeat, sent once a second whether or not the sensor works, so receivers can tell
// "node A is down" apart from "node A is up but its sensor failed".
// ID 0x700 + node number follows the CANopen convention for heartbeats.
//   bytes 0 to 3   uptime        uint32   seconds since boot
//   byte  4        sensor_ok     uint8    1 if the last sensor read succeeded, else 0
//   bytes 5 to 7   reserved, 0
static constexpr uint32_t CAN_ID_HEARTBEAT_NODE_A = 0x701;

struct CanFrame {
    uint32_t id;       // 11 bit identifier
    uint8_t  len;      // payload length in bytes, 0 to 8
    uint8_t  data[8];  // payload
};

// Caller must range check first: temperature has to fit in int16 and humidity in uint16.
inline CanFrame can_pack_reading(const BmeReading &r)
{
    CanFrame f = {};
    f.id = CAN_ID_ENV_READING;
    f.len = 8;

    uint16_t t = (uint16_t)(int16_t)r.temp_c100;  // keep the two's complement bits
    uint16_t h = (uint16_t)r.hum_c100;
    uint32_t p = r.press_pa;

    f.data[0] = t & 0xFF;
    f.data[1] = (t >> 8) & 0xFF;
    f.data[2] = h & 0xFF;
    f.data[3] = (h >> 8) & 0xFF;
    f.data[4] = p & 0xFF;
    f.data[5] = (p >> 8) & 0xFF;
    f.data[6] = (p >> 16) & 0xFF;
    f.data[7] = (p >> 24) & 0xFF;
    return f;
}

// Inverse of can_pack_reading. Node B will use the same function to decode.
inline BmeReading can_unpack_reading(const CanFrame &f)
{
    BmeReading r = {};
    r.temp_c100 = (int16_t)(f.data[0] | f.data[1] << 8);
    r.hum_c100 = (uint16_t)(f.data[2] | f.data[3] << 8);
    r.press_pa = (uint32_t)f.data[4] | (uint32_t)f.data[5] << 8 |
                 (uint32_t)f.data[6] << 16 | (uint32_t)f.data[7] << 24;
    return r;
}

inline CanFrame can_pack_heartbeat(uint32_t uptime_s, bool sensor_ok)
{
    CanFrame f = {};
    f.id = CAN_ID_HEARTBEAT_NODE_A;
    f.len = 8;
    f.data[0] = uptime_s & 0xFF;
    f.data[1] = (uptime_s >> 8) & 0xFF;
    f.data[2] = (uptime_s >> 16) & 0xFF;
    f.data[3] = (uptime_s >> 24) & 0xFF;
    f.data[4] = sensor_ok ? 1 : 0;
    return f;
}
