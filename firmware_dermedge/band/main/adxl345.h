// ADXL345 on the add-on board, I2C on SDA 21 / SCL 22 (CS tied to 3V3 for I2C mode, SDO to
// GND for address 0x53). Samples at 800 Hz (the most 400 kHz I2C carries) into the chip's
// 32-deep FIFO, which holds 40 ms: main.c reads it from its own task every 10 ms.
#pragma once
#include <stdbool.h>

#include "esp_err.h"

#include <stdint.h>

#define ADXL_MAX_BATCH 32
#define ADXL_HZ 800
#define ADXL_LSB_PER_G 256.0f  // full resolution, +-2 g

esp_err_t adxl_init(void);
// Reads everything waiting in the FIFO as raw counts (divide by ADXL_LSB_PER_G for g). Returns
// the sample count, or -1 on an I2C error (the sensor came loose). *overrun is set when the
// FIFO filled up and dropped samples since the last read.
int adxl_read(int16_t xyz[][3], int max, bool *overrun);
