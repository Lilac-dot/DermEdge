// ST7735 panel driver for the framebuffer in display.c
#include "display.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// The wiring firmware_dermedge/tft_test brought up: VCC on 5V (the V1.1 board's regulator),
// A0 on 16 (not 2: onboard LED; not 21: I2C), RESET on 4, backlight straight to 3V3
#define PIN_MOSI 23  // SDA on the module
#define PIN_SCLK 18  // SCK on the module
#define PIN_CS 5
#define PIN_DC 16  // A0 on the module
#define PIN_RST 4
#define SPI_HZ (10 * 1000 * 1000)
#define CHUNK_ROWS 16

static const char *TAG = "st7735";
static spi_device_handle_t spi;

static void send(bool data, const void *buf, size_t len) {
    if (!len) return;
    gpio_set_level(PIN_DC, data);
    spi_transaction_t t = {.length = len * 8, .tx_buffer = buf};
    ESP_ERROR_CHECK(spi_device_polling_transmit(spi, &t));
}

static void cmd(uint8_t c, const uint8_t *args, size_t n) {
    send(false, &c, 1);
    send(true, args, n);
}

#define CMD(c, ...)                         \
    do {                                    \
        const uint8_t a_[] = {__VA_ARGS__}; \
        cmd(c, a_, sizeof(a_));             \
    } while (0)

void disp_init(void) {
    gpio_config_t io = {.pin_bit_mask = (1ULL << PIN_DC) | (1ULL << PIN_RST), .mode = GPIO_MODE_OUTPUT};
    ESP_ERROR_CHECK(gpio_config(&io));
    spi_bus_config_t bus = {.mosi_io_num = PIN_MOSI, .miso_io_num = -1, .sclk_io_num = PIN_SCLK,
                            .quadwp_io_num = -1, .quadhd_io_num = -1,
                            .max_transfer_sz = DISP_W * 2 * CHUNK_ROWS};
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO));
    spi_device_interface_config_t dev = {.clock_speed_hz = SPI_HZ, .mode = 0, .spics_io_num = PIN_CS,
                                         .queue_size = 1};
    ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST, &dev, &spi));

    gpio_set_level(PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(150));
    // ST7735R "black tab" init, as in tft_test
    cmd(0x01, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(150));
    cmd(0x11, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(500));
    CMD(0xB1, 0x01, 0x2C, 0x2D);
    CMD(0xB2, 0x01, 0x2C, 0x2D);
    CMD(0xB3, 0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D);
    CMD(0xB4, 0x07);
    CMD(0xC0, 0xA2, 0x02, 0x84);
    CMD(0xC1, 0xC5);
    CMD(0xC2, 0x0A, 0x00);
    CMD(0xC3, 0x8A, 0x2A);
    CMD(0xC4, 0x8A, 0xEE);
    CMD(0xC5, 0x0E);
    cmd(0x20, NULL, 0);
    CMD(0x36, 0xA0);  // landscape, RGB order: this panel shows pink as lilac with the BGR bit set
    CMD(0x3A, 0x05);
    CMD(0xE0, 0x02, 0x1C, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2D, 0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10);
    CMD(0xE1, 0x03, 0x1D, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D, 0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10);
    cmd(0x13, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    cmd(0x29, NULL, 0);
    ESP_LOGI(TAG, "ST7735 ready");
}

void disp_flush(void) {
    CMD(0x2A, 0, 0, 0, DISP_W - 1);
    CMD(0x2B, 0, 0, 0, DISP_H - 1);
    cmd(0x2C, NULL, 0);
    for (int y = 0; y < DISP_H; y += CHUNK_ROWS) send(true, disp_framebuffer() + y * DISP_W, DISP_W * 2 * CHUNK_ROWS);
}
