// The heart and skin-temperature sensors: beat detection, HRV and conversions, and a simulator
// for testing without them. Plain C, no ESP-IDF, so the host tests check the maths.
//
//   ECG   an AD8232 with electrodes on both wrists (Lead I), sampled at 250 Hz. Each beat's R-peak
//         gives an RR interval; RR intervals that jump too far from the recent ones (a missed or
//         extra beat, a bumped lead) are counted but left out of the HRV. Every 30 s of electrode
//         contact with the hand still becomes one reading: heart rate, RMSSD and SDNN, the
//         beat-timing features the stress model uses. Short windows are valid: 30-120 s of RMSSD
//         agrees well with 5 minutes (Munoz et al. 2015, 3,387 adults).
//   temp  an LM35 on the wrist: 10 mV per degree C. One input to the stress model (distal skin
//         cools slightly under stress), not an eczema marker. The LM35 itself is within about
//         0.5-1 C, but the ESP32's ADC is off by tens of millivolts near 0.3 V, several degrees,
//         so "temp cal <C>" stores a one-point offset against a reference thermometer.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SKIN_ECG_HZ 250
#define SKIN_TEMP_MIN_C 10.0f    // outside 10-50 C the LM35 is unplugged, floating or shorted
#define SKIN_TEMP_MAX_C 50.0f
#define SKIN_TEMP_MAX_OFFSET_C 10.0f  // more than this isn't inaccuracy: check the wiring

#define ECG_REFRACTORY_MS 280    // no two beats closer than this (about 210 bpm)
#define ECG_MIN_PEAK_MV 40.0f    // ignore wiggles smaller than this
#define ECG_RR_MIN_MS 300        // 200 bpm
#define ECG_RR_MAX_MS 2000       // 30 bpm
#define ECG_RR_JUMP 0.3f         // an RR more than 30% from the recent median is an artifact

#define HRV_WINDOW_S 30          // the website's stress windows
#define HRV_MIN_BEATS 15         // fewer good intervals than this: no reading
#define HRV_MAX_BAD 0.2f         // more artifacts than this fraction: no reading
#define HRV_MIN_STILL_S 27       // seconds of the window the hand must be still
#define HRV_MAX_RR 80            // intervals kept per window (160 bpm for 30 s)

float skin_lm35_c(float mv);
// The offset that makes `measured` read `reference`; false (and *offset untouched) if it's over
// SKIN_TEMP_MAX_OFFSET_C or either value is outside 10-50 C
bool skin_temp_offset(float measured, float reference, float *offset);
bool skin_temp_plausible(float c);

// --- R-peak detector -----------------------------------------------------------------------
// Distance from a slow baseline against a slowly decaying envelope of recent peaks, so it works
// with either electrode polarity and ignores T waves (well under 60% of the R wave).
typedef struct {
    float baseline, envelope;
    bool above, primed;
    uint32_t n, last_beat;   // samples seen; sample index of the last beat (0: none yet)
    uint16_t recent[5];      // the last good RR intervals, for the artifact check
    int n_recent;
} ecg_det_t;

void ecg_det_init(ecg_det_t *d);
// One sample. Returns true on a beat; *rr_ms is the interval since the previous beat (0 for the
// first beat after the electrodes went on) and *valid whether it counts towards HRV. Electrodes
// off resets the detector.
bool ecg_det_feed(ecg_det_t *d, float mv, bool leads_on, int *rr_ms, bool *valid);
// Live heart rate from the recent good intervals, or 0
float ecg_det_bpm(const ecg_det_t *d);

// --- 30 s HRV windows -----------------------------------------------------------------------
typedef struct {
    uint16_t rr[HRV_MAX_RR];  // 0 marks an artifact, so differences skip across it
    int n_rr, n_bad, secs, secs_still;
} hrv_win_t;

typedef struct {
    float hr, rmssd, sdnn;   // bpm, ms, ms
    int beats;               // good intervals used
} hrv_t;

void hrv_win_reset(hrv_win_t *w);
void hrv_win_add_rr(hrv_win_t *w, int rr_ms, bool valid);
void hrv_win_add_second(hrv_win_t *w, bool still);
bool hrv_win_full(const hrv_win_t *w);
// The window's reading; false when it isn't usable (too few beats, too many artifacts, moving)
bool hrv_win_result(const hrv_win_t *w, hrv_t *out);

// --- simulator ------------------------------------------------------------------------------
// A synthetic Lead I ECG (QRS, T wave, baseline wander, noise) around the AD8232's mid-rail, and a
// slowly drifting wrist temperature. Calm: about 68 bpm with breathing-driven variability;
// "stressed": about 92 bpm with much less.
typedef struct {
    float since_ms, rr_ms, t_s, temp;
    uint32_t seed;
    bool stressed;
} skin_sim_t;

void skin_sim_init(skin_sim_t *s, uint32_t seed);
// One ECG sample (millivolts at the AD8232's output), advancing 1/SKIN_ECG_HZ s
float skin_sim_ecg(skin_sim_t *s);
// The wrist temperature now; call once a second
float skin_sim_temp(skin_sim_t *s);
