#include "bme280.h"

#include <inttypes.h>
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bme280";

// ---------------------------------------------------------------------------
// BME280 register map (datasheet section 5.3, "Memory map")
// ---------------------------------------------------------------------------
static constexpr uint8_t BME_ADDR       = 0x77;  // 0x76 if SDO is tied to GND
static constexpr uint8_t REG_ID         = 0xD0;  // reads 0x60 on a real BME280
static constexpr uint8_t REG_RESET      = 0xE0;  // write 0xB6 to soft reset
static constexpr uint8_t REG_CTRL_HUM   = 0xF2;  // humidity oversampling
static constexpr uint8_t REG_STATUS     = 0xF3;  // measuring and im_update bits
static constexpr uint8_t REG_CTRL_MEAS  = 0xF4;  // temp and pressure oversampling, mode
static constexpr uint8_t REG_CONFIG     = 0xF5;  // standby time, IIR filter
static constexpr uint8_t REG_DATA       = 0xF7;  // start of the 8 byte raw data block
static constexpr uint8_t REG_CALIB_A    = 0x88;  // calibration block 1, 26 bytes
static constexpr uint8_t REG_CALIB_B    = 0xE1;  // calibration block 2, 7 bytes
static constexpr uint8_t RESET_VALUE    = 0xB6;

static constexpr int I2C_TIMEOUT_MS = 100;

// Device handle for the sensor. Set by bme280_bus_init, used by the helpers.
static i2c_master_dev_handle_t s_dev;

// Bus handle, kept so bme280_recover can tear the bus down and rebuild it.
static i2c_master_bus_handle_t s_bus;

// ---------------------------------------------------------------------------
// Register access helpers. Every other function calls these.
// ---------------------------------------------------------------------------

// Read `len` bytes starting at register `reg` into `buf`.
static esp_err_t bme_read(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, I2C_TIMEOUT_MS);
}

// Write one byte `value` to register `reg`.
static esp_err_t bme_write(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = {reg, value};  // register address first, then the data byte
    return i2c_master_transmit(s_dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

// ---------------------------------------------------------------------------
// Driver steps
// ---------------------------------------------------------------------------

// Confirm we are talking to a BME280, not a BMP280.
static bool bme_check_id()
{
    uint8_t id = 0;  // bme_read fills this in
    esp_err_t err = bme_read(REG_ID, &id, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "chip ID read failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "chip ID is 0x%02X", id);
    return id == 0x60;  // 0x58 would be a BMP280
}

// Soft reset, then wait for the chip to finish loading its calibration.
static esp_err_t bme_reset()
{
    esp_err_t err = bme_write(REG_RESET, RESET_VALUE);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(10));  // chip needs a moment to restart

    // im_update (bit 0 of status) is 1 while the chip copies its calibration
    // data into the registers. Poll until it clears, give up after 10 tries.
    for (int i = 0; i < 10; i++) {
        uint8_t status = 0;
        err = bme_read(REG_STATUS, &status, 1);
        if (err != ESP_OK) {
            return err;
        }
        if ((status & 0x01) == 0) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_ERR_TIMEOUT;
}

// Factory calibration constants, named exactly as in the datasheet (section 4.2.2).
// Types matter: unsigned and signed decode the same bytes to different numbers.
struct BmeCalib {
    uint16_t dig_T1;
    int16_t  dig_T2, dig_T3;
    uint16_t dig_P1;
    int16_t  dig_P2, dig_P3, dig_P4, dig_P5, dig_P6, dig_P7, dig_P8, dig_P9;
    uint8_t  dig_H1;
    int16_t  dig_H2;
    uint8_t  dig_H3;
    int16_t  dig_H4, dig_H5;
    int8_t   dig_H6;
};
static BmeCalib s_calib;

// The chip sends 16 bit values low byte first (little endian).
// Combine two bytes into one number: high byte shifted up 8, ORed with the low byte.
static uint16_t u16_le(const uint8_t *p)
{
    return (uint16_t)(p[1] << 8 | p[0]);
}

// Same bytes, read as a signed (two's complement) number.
static int16_t s16_le(const uint8_t *p)
{
    return (int16_t)u16_le(p);
}

// Read the factory calibration constants. Every chip has its own.
static esp_err_t bme_read_calibration()
{
    uint8_t calib_a[26];

    esp_err_t err = bme_read(REG_CALIB_A, calib_a, sizeof(calib_a));

    if (err != ESP_OK) {
        return err;
    }

    uint8_t calib_b[7];

    err = bme_read(REG_CALIB_B, calib_b, sizeof(calib_b));

    if (err != ESP_OK) {
        return err;
    }

    ESP_LOG_BUFFER_HEX(TAG, calib_a, sizeof(calib_a));
    ESP_LOG_BUFFER_HEX(TAG, calib_b, sizeof(calib_b));

    // E2: turn the raw bytes into named constants.
    // Block A starts at register 0x88, so the byte index is (register - 0x88).
    s_calib.dig_T1 = u16_le(&calib_a[0]);   // 0x88, 0x89
    s_calib.dig_T2 = s16_le(&calib_a[2]);   // 0x8A, 0x8B
    s_calib.dig_T3 = s16_le(&calib_a[4]);   // 0x8C, 0x8D
    s_calib.dig_P1 = u16_le(&calib_a[6]);   // 0x8E, 0x8F
    s_calib.dig_P2 = s16_le(&calib_a[8]);
    s_calib.dig_P3 = s16_le(&calib_a[10]);
    s_calib.dig_P4 = s16_le(&calib_a[12]);
    s_calib.dig_P5 = s16_le(&calib_a[14]);
    s_calib.dig_P6 = s16_le(&calib_a[16]);
    s_calib.dig_P7 = s16_le(&calib_a[18]);
    s_calib.dig_P8 = s16_le(&calib_a[20]);
    s_calib.dig_P9 = s16_le(&calib_a[22]);  // 0x9E, 0x9F
    // calib_a[24] is register 0xA0, unused.
    s_calib.dig_H1 = calib_a[25];           // 0xA1, one byte

    // Block B starts at register 0xE1.
    s_calib.dig_H2 = s16_le(&calib_b[0]);   // 0xE1, 0xE2
    s_calib.dig_H3 = calib_b[2];            // 0xE3, one byte
    // H4 and H5 are 12 bit values that share register 0xE5, one nibble each:
    //   H4 = 0xE4 (signed, upper 8 bits) joined with the LOW nibble of 0xE5
    //   H5 = 0xE6 (signed, upper 8 bits) joined with the HIGH nibble of 0xE5
    // Multiply by 16 instead of shifting, because shifting a negative number is murky.
    s_calib.dig_H4 = (int16_t)((int8_t)calib_b[3] * 16 | (calib_b[4] & 0x0F));
    s_calib.dig_H5 = (int16_t)((int8_t)calib_b[5] * 16 | (calib_b[4] >> 4));
    s_calib.dig_H6 = (int8_t)calib_b[6];    // 0xE7, signed byte

    ESP_LOGI(TAG, "T1=%u T2=%d T3=%d", s_calib.dig_T1, s_calib.dig_T2, s_calib.dig_T3);
    ESP_LOGI(TAG, "P1=%u P2=%d P3=%d P4=%d P5=%d P6=%d P7=%d P8=%d P9=%d",
             s_calib.dig_P1, s_calib.dig_P2, s_calib.dig_P3, s_calib.dig_P4, s_calib.dig_P5,
             s_calib.dig_P6, s_calib.dig_P7, s_calib.dig_P8, s_calib.dig_P9);
    ESP_LOGI(TAG, "H1=%u H2=%d H3=%u H4=%d H5=%d H6=%d",
             s_calib.dig_H1, s_calib.dig_H2, s_calib.dig_H3, s_calib.dig_H4, s_calib.dig_H5,
             s_calib.dig_H6);

    return ESP_OK;
}

// Set oversampling and start measuring.
static esp_err_t bme_configure()
{
    // Field values from the datasheet tables. Binary 001 means "oversampling x1".
    const uint8_t OSRS_X1   = 0b001;
    const uint8_t MODE_NORMAL = 0b11;

    // ctrl_hum (0xF2): only bits 2 to 0 are used, osrs_h. No shifting needed.
    uint8_t ctrl_hum = OSRS_X1;

    // ctrl_meas (0xF4): osrs_t in bits 7 to 5, osrs_p in bits 4 to 2, mode in bits 1 to 0.
    // Shift each field up to its position, then OR them into one byte.
    uint8_t ctrl_meas = (OSRS_X1 << 5) | (OSRS_X1 << 2) | MODE_NORMAL;

    // Order matters: ctrl_hum only takes effect after the next write to ctrl_meas,
    // and writing ctrl_meas with a mode other than sleep starts measuring.
    esp_err_t err = bme_write(REG_CTRL_HUM, ctrl_hum);
    if (err != ESP_OK) {
        return err;
    }
    err = bme_write(REG_CTRL_MEAS, ctrl_meas);
    if (err != ESP_OK) {
        return err;
    }

    // Read both back to confirm the chip took our values.
    uint8_t got_hum = 0, got_meas = 0;
    err = bme_read(REG_CTRL_HUM, &got_hum, 1);
    if (err != ESP_OK) {
        return err;
    }
    err = bme_read(REG_CTRL_MEAS, &got_meas, 1);
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "ctrl_hum wrote 0x%02X read 0x%02X", ctrl_hum, got_hum);
    ESP_LOGI(TAG, "ctrl_meas wrote 0x%02X read 0x%02X", ctrl_meas, got_meas);

    return ESP_OK;
}

// Read the raw, uncompensated measurements.
static esp_err_t bme_read_raw(int32_t *adc_T, int32_t *adc_P, int32_t *adc_H)
{
    // One burst read of all 8 data bytes, so they all come from the same measurement.
    uint8_t d[8];
    esp_err_t err = bme_read(REG_DATA, d, sizeof(d));
    if (err != ESP_OK) {
        return err;
    }

    // Pressure and temperature are 20 bit, big endian (high byte first).
    // The third byte only contributes its top 4 bits, so shift it right by 4.
    //   d[0] holds bits 19..12, d[1] holds bits 11..4, d[2] top nibble holds bits 3..0
    *adc_P = ((int32_t)d[0] << 12) | ((int32_t)d[1] << 4) | (d[2] >> 4);
    *adc_T = ((int32_t)d[3] << 12) | ((int32_t)d[4] << 4) | (d[5] >> 4);

    // Humidity is 16 bit, big endian.
    *adc_H = ((int32_t)d[6] << 8) | d[7];

    // 0x80000 is the datasheet's "skipped" value: no measurement has finished yet.
    // Reject it, or the compensation math turns it into nonsense (like 71377 Pa).
    if (*adc_T == 0x80000) {
        return ESP_ERR_NOT_FINISHED;
    }

    ESP_LOGD(TAG, "raw T=%" PRId32 " P=%" PRId32 " H=%" PRId32, *adc_T, *adc_P, *adc_H);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// H: compensation. Raw ADC counts plus calibration constants become real units.
// Integer formulas from the datasheet (section 4.2.3). The chip has no floating
// point unit worth using, and integers keep results identical on every build.
// The shifts (>> n) are divisions by powers of two, chosen so intermediate
// values stay inside 32 bits.
// ---------------------------------------------------------------------------

// Temperature depends on nothing, but pressure and humidity both depend on it.
// t_fine carries the temperature result forward, so compensate T first.
static int32_t s_t_fine;

// Returns temperature in 0.01 C. 2195 means 21.95 C.
static int32_t bme_compensate_temp(int32_t adc_T)
{
    int32_t var1 = (((adc_T >> 3) - ((int32_t)s_calib.dig_T1 << 1)) * (int32_t)s_calib.dig_T2) >> 11;
    int32_t var2 = (((((adc_T >> 4) - (int32_t)s_calib.dig_T1) *
                      ((adc_T >> 4) - (int32_t)s_calib.dig_T1)) >> 12) *
                    (int32_t)s_calib.dig_T3) >> 14;
    s_t_fine = var1 + var2;
    return (s_t_fine * 5 + 128) >> 8;
}

// Returns pressure in Pa. 101325 is standard sea level pressure.
// Needs s_t_fine, so call bme_compensate_temp first.
static uint32_t bme_compensate_pressure(int32_t adc_P)
{
    int32_t var1 = (s_t_fine >> 1) - 64000;
    int32_t var2 = (((var1 >> 2) * (var1 >> 2)) >> 11) * (int32_t)s_calib.dig_P6;
    var2 = var2 + ((var1 * (int32_t)s_calib.dig_P5) << 1);
    var2 = (var2 >> 2) + ((int32_t)s_calib.dig_P4 << 16);
    var1 = (((s_calib.dig_P3 * (((var1 >> 2) * (var1 >> 2)) >> 13)) >> 3) +
            (((int32_t)s_calib.dig_P2 * var1) >> 1)) >> 18;
    var1 = ((32768 + var1) * (int32_t)s_calib.dig_P1) >> 15;
    if (var1 == 0) {
        return 0;  // avoid dividing by zero if calibration is bad
    }
    uint32_t p = (((uint32_t)(1048576 - adc_P) - (var2 >> 12))) * 3125;
    if (p < 0x80000000) {
        p = (p << 1) / (uint32_t)var1;
    } else {
        p = (p / (uint32_t)var1) * 2;
    }
    var1 = ((int32_t)s_calib.dig_P9 * (int32_t)(((p >> 3) * (p >> 3)) >> 13)) >> 12;
    var2 = ((int32_t)(p >> 2) * (int32_t)s_calib.dig_P8) >> 13;
    p = (uint32_t)((int32_t)p + ((var1 + var2 + s_calib.dig_P7) >> 4));
    return p;
}

// Returns relative humidity in 0.01 %. 5902 means 59.02 %.
// Needs s_t_fine, so call bme_compensate_temp first.
static uint32_t bme_compensate_humidity(int32_t adc_H)
{
    int32_t v = s_t_fine - 76800;
    v = (((((adc_H << 14) - ((int32_t)s_calib.dig_H4 << 20) -
            ((int32_t)s_calib.dig_H5 * v)) + 16384) >> 15) *
         (((((((v * (int32_t)s_calib.dig_H6) >> 10) *
              (((v * (int32_t)s_calib.dig_H3) >> 11) + 32768)) >> 10) + 2097152) *
           (int32_t)s_calib.dig_H2 + 8192) >> 14));
    v = v - (((((v >> 15) * (v >> 15)) >> 7) * (int32_t)s_calib.dig_H1) >> 4);
    v = v < 0 ? 0 : v;
    v = v > 419430400 ? 419430400 : v;  // clamp to 100 %
    // v >> 12 is humidity in Q22.10 fixed point, where 1024 means 1 %.
    // Convert to 0.01 %: multiply by 100, divide by 1024.
    uint32_t q22_10 = (uint32_t)(v >> 12);
    return (q22_10 * 100) >> 10;
}

// Create the I2C bus (SDA on GPIO 8, SCL on GPIO 9) and register the sensor on it.
esp_err_t bme280_bus_init()
{
    i2c_master_bus_config_t cfg = {};
    cfg.i2c_port = I2C_NUM_0;
    cfg.sda_io_num = GPIO_NUM_8;
    cfg.scl_io_num = GPIO_NUM_9;
    cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    cfg.glitch_ignore_cnt = 7;                  // ignore pulses shorter than 7 cycles
    cfg.flags.enable_internal_pullup = true;    // the breakout has its own as well

    esp_err_t err = i2c_new_master_bus(&cfg, &s_bus);
    if (err != ESP_OK) {
        return err;
    }

    // Describe the sensor as a device on the bus: address, address size, clock speed.
    i2c_device_config_t device_config = {};
    device_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    device_config.device_address = BME_ADDR;
    device_config.scl_speed_hz = 100000;
    return i2c_master_bus_add_device(s_bus, &device_config, &s_dev);
}

// Recover from a stuck bus. i2c_master_bus_reset alone clears the bus lines, but the
// driver can stay in an error state where every transfer fails. Tearing the bus down
// and creating it again is the software equivalent of a reboot, scoped to I2C.
esp_err_t bme280_recover()
{
    i2c_master_bus_reset(s_bus);       // clock out any half finished transfer first
    i2c_master_bus_rm_device(s_dev);
    i2c_del_master_bus(s_bus);
    return bme280_bus_init();
}

// Full sensor bring-up: verify the chip, reset it, load calibration, start measuring.
// Used at boot and again after a bus recovery, since a glitch can leave the sensor
// in an unknown state.
esp_err_t bme280_init()
{
    if (!bme_check_id()) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = bme_reset();
    if (err != ESP_OK) {
        return err;
    }
    err = bme_read_calibration();
    if (err != ESP_OK) {
        return err;
    }
    return bme_configure();
}

void bme280_scan_bus()
{
    // Probe every legal 7 bit address and log which ones ACK.
    int found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(s_bus, addr, 50) == ESP_OK) {
            ESP_LOGI(TAG, "found device at 0x%02X", addr);
            found++;
        }
    }
    ESP_LOGI(TAG, "scan done, %d device(s)", found);
}

esp_err_t bme280_read(BmeReading *out)
{
    int32_t adc_T, adc_P, adc_H;
    esp_err_t err = bme_read_raw(&adc_T, &adc_P, &adc_H);
    if (err != ESP_OK) {
        return err;
    }
    // Temperature first: it sets s_t_fine, which the other two use.
    out->temp_c100 = bme_compensate_temp(adc_T);
    out->press_pa = bme_compensate_pressure(adc_P);
    out->hum_c100 = bme_compensate_humidity(adc_H);
    return ESP_OK;
}
