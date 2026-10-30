// The heart and skin-temperature sensors on the ESP32: a task samples the AD8232's ECG
// (GPIO 32) and its lead-off pins at 250 Hz, finds the beats (skin.c), and reads the LM35
// (GPIO 35) on every sample, averaged over each second. Both are on ADC1, which keeps working
// while Bluetooth is on. Once a second it posts a skin_reading_t: the beats of that second,
// whether the electrodes were on, and the temperature. main.c turns 30 s of them into HRV readings.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define SKIN_ECG_GPIO 32
#define SKIN_LO_PLUS_GPIO 33   // high while an electrode is off
#define SKIN_LO_MINUS_GPIO 34
#define SKIN_TEMP_GPIO 35
#define SKIN_MAX_BEATS 4       // per second: 240 bpm

typedef struct {
    float c;                   // degrees C, averaged over the second
    bool temp_ok;              // in range and steady for 3 s
    bool temp_settling;        // in range, not steady yet
    bool ecg_on;               // electrodes on for (almost) the whole second
    float hr_bpm;              // live heart rate from the recent beats, 0 if none
    int n_beats;
    uint16_t rr_ms[SKIN_MAX_BEATS];  // intervals ending at this second's beats (0: the first beat)
    bool rr_valid[SKIN_MAX_BEATS];
    bool sim;                  // simulated ("sim on"), not the sensors
    float ecg_mv, temp_mv;     // what the pins read, for "skin on"
} skin_reading_t;

// Starts the task; it posts one skin_reading_t to `out` every second
void skin_adc_start(QueueHandle_t out);
void skin_set_sim(bool on);
void skin_set_sim_stressed(bool on);
void skin_set_debug(bool on);  // a line a second on the serial port: pins, values, verdicts
// One-point LM35 calibration: with the sensor settled next to a reference thermometer reading
// `reference_c`, averages the next 10 s and stores the offset in NVS. Blocks for 10 s. False if the
// offset would be over 10 C (wiring, not inaccuracy) or the readings aren't steady.
bool skin_temp_calibrate(float reference_c, float *offset, float *measured);
void skin_temp_cal_clear(void);
float skin_temp_cal_offset(void);
