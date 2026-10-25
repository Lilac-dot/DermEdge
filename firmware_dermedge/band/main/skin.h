// Skin conductance (GSR) and skin temperature: conversions from ADC millivolts, and a simulator
// for testing without the sensors. Plain C, no ESP-IDF, so the host tests check the maths.
//
//   GSR   a Grove GSR module on GPIO 32. Its own formula, R = (1024 + 2a) * 10000 / (no_skin - a)
//         ohms for a 10-bit reading a, where no_skin (about 512) is the reading with nothing
//         touching the electrodes; calibrate it per module with "gsr cal". 4 Hz, the rate the
//         website's stress model (web/models/stress_gsr.json) was trained at.
//   temp  a 10 kOhm NTC thermistor (B 3950) on GPIO 34: 3V3 -> 10 kOhm -> GPIO 34 -> NTC -> GND.
//         Beta equation 1/T = 1/T25 + ln(R / R25) / B. Shown, not scored: in the stress model's
//         test it drifted with time on the wrist and made stress detection worse.
// From archive/gsr_wristband_wifi/main/signals.c.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SKIN_GSR_HZ 4
#define SKIN_SUPPLY_MV 3300.0f
#define SKIN_GSR_NO_SKIN 512.0f  // default no-skin reading, on the module's 10-bit scale
#define SKIN_NTC_SERIES_R 10000.0f
#define SKIN_NTC_R25 10000.0f
#define SKIN_NTC_BETA 3950.0f
#define SKIN_GSR_MIN_US 0.05f    // below this the electrodes aren't on skin
#define SKIN_GSR_MAX_US 60.0f    // above this the electrodes are touching each other
#define SKIN_TEMP_MIN_C 10.0f    // outside 10-50 C the thermistor is unplugged or shorted
#define SKIN_TEMP_MAX_C 50.0f

// The Grove module's 10-bit reading from a voltage on a supply_mv rail (it's ratiometric)
float skin_gsr_reading(float mv, float supply_mv);
// Microsiemens, or 0 when the electrodes aren't on skin (reading at or above no_skin)
float skin_gsr_us(float mv, float supply_mv, float no_skin);
// Degrees C, or a value outside 10-50 when the thermistor is open or shorted
float skin_ntc_c(float mv, float supply_mv, float r_series, float r25, float beta);
bool skin_gsr_plausible(float us);
bool skin_temp_plausible(float c);

// Simulated wearer for testing without sensors: calm skin conductance with the occasional
// response, slowly drifting skin temperature. "stressed" raises conductance and responses.
typedef struct {
    float level, scr, temp;
    uint32_t seed;
    bool stressed;
} skin_sim_t;

void skin_sim_init(skin_sim_t *s, uint32_t seed);
// One 4 Hz step
void skin_sim_step(skin_sim_t *s, float *us, float *c);
