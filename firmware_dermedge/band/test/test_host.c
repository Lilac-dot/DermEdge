// Host tests for the band's plain-C logic:  sh test/run.sh
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "../main/app_sm.h"
#include "../main/scratch.h"
#include "../main/touch.h"

static int failures;
#define CHECK(cond, ...)                                    \
    do {                                                    \
        if (!(cond)) {                                      \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);   \
            printf(__VA_ARGS__);                            \
            printf("\n");                                   \
            failures++;                                     \
        }                                                   \
    } while (0)

// --- scratch detector ------------------------------------------------------------------
static float unit(void) { return (float)((double)rand() / RAND_MAX); }
static float noise(float amp) { return amp * (unit() * 2 - 1); }

typedef struct {
    int events;
    float first_t, first_dur;
} run_t;

// Runs the detector on `seconds` of motion from gen(t) (gravity already included)
static run_t run(float seconds, void (*gen)(float t, float *a)) {
    scratch_t s;
    scratch_reset(&s);
    run_t r = {0};
    scratch_motion_t m;
    scratch_event_t e;
    for (int i = 0; i < (int)(seconds * SCRATCH_FS); i++) {
        float a[3];
        gen((float)i / SCRATCH_FS, a);
        if (scratch_feed(&s, a[0], a[1], a[2], &m, &e) & SCRATCH_GOT_EVENT) {
            if (!r.events) r.first_t = e.t, r.first_dur = e.duration_s;
            r.events++;
        }
    }
    if (scratch_flush(&s, &e)) {
        if (!r.events) r.first_t = e.t, r.first_dur = e.duration_s;
        r.events++;
    }
    return r;
}

#define PI 3.14159265f
static void rest(float t, float *a) {
    (void)t; a[0] = noise(0.01f); a[1] = noise(0.01f); a[2] = 1 + noise(0.01f); }
// 5 strokes a second for 6 s in the middle of 12 s of rest, the wrist tilted
static void scratching(float t, float *a) {
    float s = t >= 3 && t < 9 ? 0.35f * sinf(2 * PI * 5 * t) : 0;
    a[0] = 0.3f + s + noise(0.03f); a[1] = 0.2f + 0.3f * s + noise(0.03f); a[2] = 0.93f + noise(0.03f);
}
static void fast_scratching(float t, float *a) {
    float s = t >= 2 && t < 10 ? 0.2f * sinf(2 * PI * 7 * t) : 0;
    a[0] = s + noise(0.02f); a[1] = noise(0.02f); a[2] = 1 + noise(0.02f);
}
// 20 s of non-stop 5 Hz scratching: must stay one episode, not split where the 2nd harmonic wins
static void long_scratching(float t, float *a) {
    float s = 0.35f * sinf(2 * PI * 5 * t);
    a[0] = 0.3f + s + noise(0.03f); a[1] = 0.2f + 0.3f * s + noise(0.03f); a[2] = 0.93f + noise(0.03f);
}
// arm swing at 1 Hz and footsteps at 2 Hz
static void walking(float t, float *a) {
    a[0] = 0.6f * sinf(2 * PI * 1 * t) + noise(0.05f);
    a[1] = 0.2f * sinf(2 * PI * 2 * t + 0.4f) + noise(0.05f);
    a[2] = 1 + 0.25f * fabsf(sinf(2 * PI * 1 * t)) + noise(0.05f);
}
// small irregular taps
static void typing(float t, float *a) {
    static float next, until;
    if (t == 0) next = until = 0;
    if (t >= next) { until = t + 0.03f; next = t + 0.08f + unit() * 0.35f; }
    float tap = t < until ? 0.08f : 0;
    a[0] = noise(0.01f); a[1] = noise(0.01f); a[2] = 1 + tap + noise(0.01f);
}
static void jolt(float t, float *a) {
    float j = t >= 5 && t < 5.15f ? 2.0f : 0;
    a[0] = j + noise(0.01f); a[1] = noise(0.01f); a[2] = 1 + noise(0.01f);
}

static void test_scratch(void) {
    puts("scratch detector");
    run_t r = run(20, rest);
    CHECK(r.events == 0, "rest: %d events", r.events);
    r = run(12, scratching);
    CHECK(r.events == 1, "scratching: %d events", r.events);
    CHECK(fabsf(r.first_t - 3) <= 1.5f, "scratching starts at %.1f s, want ~3", r.first_t);
    CHECK(r.first_dur >= 4 && r.first_dur <= 8, "scratching lasts %.1f s, want ~6", r.first_dur);
    r = run(12, fast_scratching);
    CHECK(r.events == 1, "fast scratching: %d events", r.events);
    r = run(20, long_scratching);
    CHECK(r.events == 1 && r.first_dur >= 16, "long scratching: %d events, first %.1f s", r.events, r.first_dur);
    r = run(30, walking);
    CHECK(r.events == 0, "walking: %d events", r.events);
    r = run(30, typing);
    CHECK(r.events == 0, "typing: %d events", r.events);
    r = run(10, jolt);
    CHECK(r.events == 0, "jolt: %d events", r.events);
}

// --- touch gestures --------------------------------------------------------------------
// Plays a pattern of (level, ms) steps through the decoder at 10 ms polling
static int gestures(const int *steps, int n, touch_gesture_t *out) {
    touch_t t;
    pad_init(&t);
    uint32_t now = 0;
    int got = 0;
    for (int i = 0; i < n; i += 2)
        for (int ms = 0; ms < steps[i + 1]; ms += 10, now += 10) {
            touch_gesture_t g = pad_feed(&t, steps[i], now);
            if (g != TOUCH_NONE) out[got++] = g;
        }
    return got;
}

static void test_touch(void) {
    puts("touch gestures");
    touch_gesture_t g[8];
    int tap[] = {0, 100, 1, 150, 0, 600};
    CHECK(gestures(tap, 6, g) == 1 && g[0] == TOUCH_TAP, "single tap");
    int dbl[] = {0, 100, 1, 120, 0, 150, 1, 120, 0, 600};
    CHECK(gestures(dbl, 10, g) == 1 && g[0] == TOUCH_DOUBLE_TAP, "double tap");
    int lng[] = {0, 100, 1, 2600, 0, 600};
    CHECK(gestures(lng, 6, g) == 1 && g[0] == TOUCH_LONG_PRESS, "long press fires once, no tap after");
    int medium[] = {0, 100, 1, 900, 0, 600};
    CHECK(gestures(medium, 6, g) == 0, "a 0.9 s hold is nothing");
    int glitch[] = {0, 100, 1, 10, 0, 100, 1, 10, 0, 600};
    CHECK(gestures(glitch, 10, g) == 0, "10 ms blips are debounced away");
    int two[] = {0, 100, 1, 120, 0, 600, 1, 120, 0, 600};
    CHECK(gestures(two, 10, g) == 2 && g[0] == TOUCH_TAP && g[1] == TOUCH_TAP, "two separate taps");
}

// --- state machine ---------------------------------------------------------------------
static void test_sm(void) {
    puts("state machine");
    sm_t sm;
    sm_init(&sm);
    CHECK(sm_handle(&sm, EV_SENSOR_FAIL, 0) & FX_REDRAW && sm.state == ST_ERROR, "sensor fail -> error");
    sm_handle(&sm, EV_SENSOR_OK, 0);
    CHECK(sm.state == ST_PAIRING, "sensor back, no link -> pairing");
    sm_handle(&sm, EV_PASSKEY, 123456);
    CHECK(sm.passkey == 123456, "passkey shown");
    sm_handle(&sm, EV_LINK_UP, 0);
    CHECK(sm.state == ST_READY && sm.passkey == 0, "link -> ready, code hidden");

    int fx = sm_handle(&sm, EV_CMD_START, 0);
    CHECK(fx & FX_START_REC && sm.state == ST_RECORDING && sm.source == SRC_WEBSITE, "website start");
    CHECK(sm_handle(&sm, EV_CMD_START, 0) == 0, "start while recording does nothing");
    sm_handle(&sm, EV_LINK_DOWN, 0);
    CHECK(sm.state == ST_RECORDING && !sm.linked, "losing the link keeps recording");
    sm.outbox = 12;
    fx = sm_handle(&sm, EV_LONG_PRESS, 0);
    CHECK(fx & FX_STOP_REC && sm.state == ST_PAIRING, "stop offline -> pairing, readings wait");
    sm_handle(&sm, EV_LINK_UP, 0);
    CHECK(sm.state == ST_SYNCING, "link back with readings waiting -> syncing");
    sm.outbox = 0;
    sm_handle(&sm, EV_OUTBOX_EMPTY, 0);
    CHECK(sm.state == ST_READY, "sent -> ready");

    fx = sm_handle(&sm, EV_LONG_PRESS, 0);
    CHECK(fx & FX_START_REC && sm.source == SRC_TOUCH && sm.recording == 2, "touch start, 2nd recording");
    fx = sm_handle(&sm, EV_CMD_STOP, 0);
    CHECK(fx & FX_STOP_REC && sm.state == ST_READY, "website stop with nothing queued -> ready");

    sm_handle(&sm, EV_ALERT, 0);
    CHECK(sm.alert, "alert shows");
    CHECK(sm_handle(&sm, EV_TAP, 0) == FX_REDRAW && sm.alert, "tap doesn't dismiss");
    fx = sm_handle(&sm, EV_DOUBLE_TAP, 0);
    CHECK(!sm.alert && fx & FX_ALERT_DISMISSED, "double tap dismisses and tells the website");
    CHECK(sm_handle(&sm, EV_DOUBLE_TAP, 0) == 0, "double tap without an alert does nothing");

    sm_init(&sm);
    sm_handle(&sm, EV_SENSOR_OK, 0);
    fx = sm_handle(&sm, EV_LONG_PRESS, 0);
    CHECK(fx & FX_START_REC && sm.state == ST_RECORDING, "can record offline from pairing");
}

int main(void) {
    srand(1);
    test_scratch();
    test_touch();
    test_sm();
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures != 0;
}
