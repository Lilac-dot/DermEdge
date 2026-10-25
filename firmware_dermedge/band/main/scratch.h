// Scratch detector for a wrist accelerometer sampled at 100 Hz. Plain C, no ESP-IDF, so the
// host tests in test/ run it on synthetic motion.
//
// Scratching is a fast, rhythmic back-and-forth: 2.5-8 strokes a second, kept up for seconds.
// Every second, the last 2 s window is checked for
//   strength    RMS of the motion with gravity removed, between SCRATCH_RMS_MIN and _MAX g
//   rhythm      autocorrelation peak of the most active axis at a lag of 2.5-8 Hz
// and a run of scratchy windows becomes one episode. Walking (about 1-2 Hz, big swings),
// typing (small and irregular) and single jolts fail one test or the other.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SCRATCH_FS 100      // Hz
#define SCRATCH_WIN 200     // 2 s window
#define SCRATCH_HOP 100     // checked every 1 s
#define SCRATCH_RMS_MIN 0.04f
#define SCRATCH_RMS_MAX 1.5f
#define SCRATCH_PERIODIC 0.45f  // normalised autocorrelation that counts as rhythmic
#define SCRATCH_LAG_MIN 12      // 8.3 Hz
#define SCRATCH_LAG_MAX 40      // 2.5 Hz
#define SCRATCH_START_WINDOWS 2 // scratchy windows in a row that start an episode
#define SCRATCH_END_WINDOWS 2   // calm windows in a row that end it

typedef struct {
    float t;               // s since reset, at the end of the window
    float activity;        // RMS of the gravity-free motion, g
    float scratch_energy;  // activity weighted by how rhythmic it is in the scratch band
    float freq_hz;         // dominant stroke rate, 0 if not rhythmic
    bool scratchy;
} scratch_motion_t;

typedef struct {
    float t;           // s since reset, when the episode started
    float duration_s;
    float intensity;   // mean activity over the episode, g
} scratch_event_t;

typedef struct {
    float g[3];                    // gravity estimate per axis
    float buf[SCRATCH_WIN][3];     // gravity-free samples, circular
    int head, filled, since_hop;
    uint32_t n;                    // samples since reset
    bool in_episode;
    int run_on, run_off;
    float ep_start, ep_last, ep_sum;
    int ep_windows;
    uint32_t episodes;
} scratch_t;

enum { SCRATCH_GOT_MOTION = 1, SCRATCH_GOT_EVENT = 2 };

void scratch_reset(scratch_t *s);
// Feeds one sample in g. Returns SCRATCH_GOT_* flags: a motion summary every second, and an
// event when an episode has just ended.
int scratch_feed(scratch_t *s, float x, float y, float z, scratch_motion_t *motion,
                 scratch_event_t *event);
// Ends an open episode (when recording stops). Returns true and fills event if there was one.
bool scratch_flush(scratch_t *s, scratch_event_t *event);
