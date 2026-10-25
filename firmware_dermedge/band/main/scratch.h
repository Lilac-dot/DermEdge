// Scratch detector for an ADXL345 on the back of the hand, sampled at 800 Hz. Plain C, no
// ESP-IDF, so the host tests in test/ and the tuning tools in tools/ run the same code.
//
// Scratching has two parts (Chun et al., Sci. Adv. 7, eabf9405, 2021, the ADAM sensor):
//   motion     the hand's back-and-forth, 2.5-8 strokes a second. Waving looks the same.
//   vibration  nails or fingertips rubbing skin, 80-400 Hz. Waving has none; finger-only
//              scratching has little motion but plenty of vibration.
// Every second, the last 2 s of motion and 1 s of vibration become a feature vector:
//   rms        motion RMS with gravity removed (decimated to 100 Hz)
//   rhythm     autocorrelation peak of the most active axis at 2.5-8 Hz
//   band[0..6] log10 power in 2-5, 5-20, 20-50, 50-80, 80-140, 140-250, 250-400 Hz (all axes)
//   vib_rhythm autocorrelation peak of the 80-400 Hz envelope at 2.5-8 Hz (strokes in the vibration)
//   vib_ratio  80-400 Hz power over 2-400 Hz power
// A logistic model trained on recordings (scratch_model.h, from tools/tune.py) scores it; until
// one is trained, a hand-set rule does: enough vibration, pulsing or moving in the stroke rhythm.
// Runs of scratchy seconds become episodes.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SCRATCH_FS 800              // Hz, the ADXL345's rate on 400 kHz I2C
#define SCRATCH_DECIM 8             // motion runs at 100 Hz
#define SCRATCH_LF_FS (SCRATCH_FS / SCRATCH_DECIM)
#define SCRATCH_LF_WIN 200          // 2 s of motion
#define SCRATCH_HF_WIN 800          // 1 s of vibration
#define SCRATCH_FFT 1024
#define SCRATCH_HOP 800             // a decision every second
#define SCRATCH_RMS_MAX 1.5f        // above this it's a jolt or a fall, not scratching
#define SCRATCH_LAG_MIN 12          // 8.3 Hz at 100 Hz
#define SCRATCH_LAG_MAX 40          // 2.5 Hz
#define SCRATCH_START_WINDOWS 2     // scratchy seconds in a row that start an episode
#define SCRATCH_END_WINDOWS 2       // calm seconds in a row that end it
#define SCRATCH_N_BANDS 7
#define SCRATCH_N_FEATURES (SCRATCH_N_BANDS + 4)

// Hand-set rule, used until a model is trained on real recordings
#define SCRATCH_RULE_VIB_MIN 1e-4f  // 80-400 Hz power (g^2): about 0.01 g RMS of vibration
#define SCRATCH_RULE_RHYTHM 0.45f   // motion or vibration-envelope autocorrelation

enum {  // feature order, shared with tools/tune.py
    F_RMS, F_RHYTHM, F_BAND0, F_VIB_RHYTHM = F_BAND0 + SCRATCH_N_BANDS, F_VIB_RATIO,
};

typedef struct {
    float t;               // s since reset, at the end of the window
    float activity;        // motion RMS, g
    float scratch_energy;  // vibration RMS in 80-400 Hz, g
    float freq_hz;         // stroke rate from motion or vibration, 0 if not rhythmic
    float p;               // probability of scratching (the model's, or the rule's 0/1)
    bool scratchy;
    float f[SCRATCH_N_FEATURES];
} scratch_motion_t;

typedef struct {
    float t;           // s since reset, when the episode started
    float duration_s;
    float intensity;   // mean vibration RMS over the episode, g
} scratch_event_t;

typedef struct {
    float g[3];                         // gravity estimate per axis (at 100 Hz)
    float acc[3];                       // sum of the current decimation block
    int acc_n;
    float lf[SCRATCH_LF_WIN][3];        // gravity-free motion, 100 Hz, circular
    int lf_head, lf_filled;
    float hf[SCRATCH_HF_WIN][3];        // raw samples for the spectrum, 800 Hz, circular
    int hf_head, hf_filled;
    float hp_state[3][4];               // 80 Hz high-pass biquad state per axis
    float env[SCRATCH_LF_WIN];          // 80-400 Hz envelope, 100 Hz, circular
    float env_acc;
    int since_hop;
    uint32_t n;                         // samples since reset
    bool in_episode;
    int run_on, run_off;
    float ep_start, ep_last, ep_sum;
    int ep_windows;
    uint32_t episodes;
} scratch_t;

enum { SCRATCH_GOT_MOTION = 1, SCRATCH_GOT_EVENT = 2 };

void scratch_reset(scratch_t *s);
// Feeds one 800 Hz sample in g. Returns SCRATCH_GOT_* flags: a summary every second, and an
// event when an episode has just ended.
int scratch_feed(scratch_t *s, float x, float y, float z, scratch_motion_t *motion, scratch_event_t *event);
// Ends an open episode (when recording stops). Returns true and fills event if there was one.
bool scratch_flush(scratch_t *s, scratch_event_t *event);
