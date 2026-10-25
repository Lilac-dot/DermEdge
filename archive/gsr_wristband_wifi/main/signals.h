// Sensor sources for the wristband: real where a sensor is configured, simulated otherwise.
// Units follow the Empatica E4 / WESAD data the stress model was trained on:
// EDA in microsiemens, skin temperature in degrees C, BVP as a unitless PPG wave,
// accelerometer in 1/64 g per axis.
#pragma once

#include <stdbool.h>
#include <stdint.h>

void signals_init(void);

// Simulated physiology follows this; the BOOT button flips it.
void signals_set_stressed(bool stressed);
bool signals_stressed(void);

// One sample each, called at the channel's own rate from the 64 Hz sampler.
float signals_eda(float t);   // 4 Hz
float signals_temp(float t);  // 4 Hz
float signals_bvp(float t);   // 64 Hz
void signals_acc(float t, int16_t out[3]);  // 32 Hz

// Moisture patch: mean capacitance-like reading over the eczema patch and over healthy
// reference skin (higher = more moisture). Returns false when no patch is configured.
bool signals_moisture(float *patch, float *reference);

// True when the value is synthetic, for the start-up log.
bool signals_gsr_simulated(void);
bool signals_temp_simulated(void);
