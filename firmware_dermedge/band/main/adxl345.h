// ADXL345 on the add-on board, I2C on SDA 21 / SCL 22 (CS tied to 3V3 for I2C mode, SDO to
// GND for address 0x53). Samples at 100 Hz into the chip's 32-deep FIFO, so a slow screen
// redraw never drops samples.
#pragma once
#include <stdbool.h>

#include "esp_err.h"

#define ADXL_MAX_BATCH 32

esp_err_t adxl_init(void);
// Reads everything waiting in the FIFO into xyz (g). Returns the sample count, or -1 on an
// I2C error (the sensor came loose).
int adxl_read(float xyz[][3], int max);
