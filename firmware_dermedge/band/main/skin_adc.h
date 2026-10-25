// The skin sensors on the ESP32: a 4 Hz task reading the Grove GSR module (GPIO 32) and the
// NTC thermistor (GPIO 34) on ADC1, which keeps working while Bluetooth is on. Conversions are in
// skin.c. A sensor that isn't plugged in, or electrodes that aren't on skin, give readings marked
// not ok, which main.c doesn't send: zeros would spoil the website's 30 s stress windows.
#pragma once
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define SKIN_GSR_GPIO 32
#define SKIN_TEMP_GPIO 34

typedef struct {
    float us, c;            // microsiemens, degrees C
    bool gsr_ok, temp_ok;   // on skin / plugged in, and readings worth sending
    bool gsr_connected;     // a module is driving the pin (it may be off the skin)
    bool temp_settling;     // a reading in range that isn't steady for 3 s yet
    bool sim;               // simulated ("sim on"), not the sensors
    float gsr_mv, temp_mv;  // what the pins read, for "skin on"
} skin_reading_t;

// Starts the task; it posts one skin_reading_t to `out` every 250 ms
void skin_adc_start(QueueHandle_t out);
void skin_set_sim(bool on);
void skin_set_sim_stressed(bool on);
void skin_set_debug(bool on);  // a line a second on the serial port: pins, values, verdicts
// Stores the GSR's current reading as its no-skin reading (electrodes off!). False if implausible.
bool skin_gsr_calibrate(float *no_skin);
