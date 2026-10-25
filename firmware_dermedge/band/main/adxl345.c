#include "adxl345.h"

#include "driver/i2c_master.h"
#include "esp_log.h"

#define PIN_SDA 21
#define PIN_SCL 22
#define I2C_TIMEOUT_MS 50
#define REG_DEVID 0x00
#define REG_BW_RATE 0x2C
#define REG_POWER_CTL 0x2D
#define REG_DATA_FORMAT 0x31
#define REG_DATAX0 0x32
#define REG_FIFO_CTL 0x38
#define REG_FIFO_STATUS 0x39
#define REG_INT_SOURCE 0x30

static const char *TAG = "adxl345";
static i2c_master_bus_handle_t bus;
static i2c_master_dev_handle_t dev;

static esp_err_t wr(uint8_t reg, uint8_t v) {
    uint8_t b[2] = {reg, v};
    return i2c_master_transmit(dev, b, 2, I2C_TIMEOUT_MS);
}

static esp_err_t rd(uint8_t reg, uint8_t *out, size_t n) {
    return i2c_master_transmit_receive(dev, &reg, 1, out, n, I2C_TIMEOUT_MS);
}

esp_err_t adxl_init(void) {
    if (!bus) {
        i2c_master_bus_config_t cfg = {.i2c_port = I2C_NUM_0, .sda_io_num = PIN_SDA, .scl_io_num = PIN_SCL,
                                       .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
                                       .flags.enable_internal_pullup = true};
        ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &bus));
    }
    if (dev) {
        i2c_master_bus_rm_device(dev);
        dev = NULL;
    }
    // 0x53 with SDO to GND, 0x1D with SDO high
    static const uint8_t addrs[] = {0x53, 0x1D};
    for (size_t i = 0; i < sizeof(addrs); i++) {
        if (i2c_master_probe(bus, addrs[i], I2C_TIMEOUT_MS) != ESP_OK) continue;
        i2c_device_config_t d = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addrs[i],
                                 .scl_speed_hz = 400000};  // 800 Hz needs 400 kHz
        ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &d, &dev));
        uint8_t id = 0;
        if (rd(REG_DEVID, &id, 1) != ESP_OK || id != 0xE5) {
            ESP_LOGW(TAG, "0x%02X answers but its ID is 0x%02X, not an ADXL345", addrs[i], id);
            i2c_master_bus_rm_device(dev);
            dev = NULL;
            continue;
        }
        esp_err_t err = wr(REG_POWER_CTL, 0x00);  // standby while configuring
        if (!err) err = wr(REG_BW_RATE, 0x0D);   // 800 Hz
        if (!err) err = wr(REG_DATA_FORMAT, 0x08);  // full resolution, +-2 g: 256 LSB/g
        if (!err) err = wr(REG_FIFO_CTL, 0x80);  // stream mode: keeps the newest 32
        if (!err) err = wr(REG_POWER_CTL, 0x08);  // measure
        if (!err) ESP_LOGI(TAG, "ADXL345 at 0x%02X, %d Hz", addrs[i], ADXL_HZ);
        return err;
    }
    return ESP_ERR_NOT_FOUND;
}

int adxl_read(int16_t xyz[][3], int max, bool *overrun) {
    if (!dev) return -1;
    uint8_t st, src;
    if (rd(REG_INT_SOURCE, &src, 1) != ESP_OK || rd(REG_FIFO_STATUS, &st, 1) != ESP_OK) return -1;
    *overrun = src & 0x01;
    int n = st & 0x3F;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) {
        uint8_t raw[6];  // one multi-byte read pops one FIFO entry
        if (rd(REG_DATAX0, raw, 6) != ESP_OK) return -1;
        for (int k = 0; k < 3; k++) xyz[i][k] = (int16_t)(raw[2 * k + 1] << 8 | raw[2 * k]);
    }
    return n;
}
