// Host tests for the band's plain-C logic:  sh test/run.sh
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "../main/app_sm.h"
#include "../main/scratch.h"
#include "../main/skin.h"
#include "../main/touch.h"
#include "../main/ui_flow.h"

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
// Synthetic 800 Hz signals with gravity included. Vibration is broadband noise, which puts most
// of its power above 80 Hz; the ADXL345's own noise is about 4 mg RMS.
static float unit(void) { return (float)((double)rand() / RAND_MAX); }
static float noise(float amp) { return amp * (unit() * 2 - 1); }
#define SENSOR 0.007f

typedef struct {
    int events, scratchy_windows, windows;
    float first_t, first_dur;
} run_t;

// Runs the detector on `seconds` of motion from gen(t)
static run_t run(float seconds, void (*gen)(float t, float *a)) {
    scratch_t s;
    scratch_reset(&s);
    run_t r = {0};
    scratch_motion_t m;
    scratch_event_t e;
    for (int i = 0; i < (int)(seconds * SCRATCH_FS); i++) {
        float a[3];
        gen((float)i / SCRATCH_FS, a);
        int got = scratch_feed(&s, a[0], a[1], a[2], &m, &e);
        if (got & SCRATCH_GOT_MOTION) r.windows++, r.scratchy_windows += m.scratchy;
        if (got & SCRATCH_GOT_EVENT) {
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
static void sensor_only(float *a) { a[0] = noise(SENSOR); a[1] = noise(SENSOR); a[2] = 1 + noise(SENSOR); }
static void rest(float t, float *a) { (void)t; sensor_only(a); }
// Nails on skin at 5 strokes a second from 3 to 9 s: the hand moves, and the pull stroke rubs
static void scratching(float t, float *a) {
    sensor_only(a);
    if (t < 3 || t >= 9) return;
    float ph = sinf(2 * PI * 5 * t), vib = ph > 0 ? noise(0.08f) : 0;
    a[0] += 0.3f + 0.35f * ph + vib; a[1] += 0.2f + 0.1f * ph + vib * 0.5f; a[2] += -0.07f + vib;
}
// 20 s non-stop: must stay one episode
static void long_scratching(float t, float *a) {
    sensor_only(a);
    float ph = sinf(2 * PI * 5 * t), vib = ph > 0 ? noise(0.08f) : 0;
    a[0] += 0.35f * ph + vib; a[2] += vib;
}
// Fingertips only, 3.5 strokes a second: the hand barely moves, but the skin buzzes in rhythm
static void finger_scratching(float t, float *a) {
    sensor_only(a);
    if (t < 2 || t >= 10) return;
    float ph = sinf(2 * PI * 3.5f * t), vib = ph > 0 ? noise(0.05f) : 0;
    a[0] += 0.015f * ph + vib; a[1] += vib * 0.7f; a[2] += vib;
}
// Waving at 5 Hz: the motion a 100 Hz smartwatch takes for scratching, with no skin contact
static void waving(float t, float *a) {
    sensor_only(a);
    float ph = sinf(2 * PI * 5 * t);
    a[0] += 0.5f * ph; a[1] += 0.15f * cosf(2 * PI * 5 * t);
}
// Arm swing at 1 Hz and a sharp heel strike every half second
static void walking(float t, float *a) {
    sensor_only(a);
    float strike = fmodf(t, 0.5f) < 0.02f ? noise(0.15f) : 0;
    a[0] += 0.6f * sinf(2 * PI * 1 * t) + strike; a[1] += 0.2f * sinf(2 * PI * 2 * t + 0.4f);
    a[2] += 0.25f * fabsf(sinf(2 * PI * 1 * t)) + strike;
}
// Key presses: short clicks of vibration at irregular gaps, almost no hand motion
static void typing(float t, float *a) {
    static float next, until;
    if (t == 0) next = until = 0;
    if (t >= next) { until = t + 0.03f; next = t + 0.09f + unit() * 0.4f; }
    sensor_only(a);
    float click = t < until ? noise(0.1f) : 0;
    a[0] += click * 0.5f; a[2] += 0.03f * (t < until) + click;
}
static void jolt(float t, float *a) {
    sensor_only(a);
    if (t >= 5 && t < 5.15f) a[0] += 2.0f + noise(0.3f), a[2] += noise(0.3f);
}

static void test_scratch(void) {
    puts("scratch detector");
    run_t r = run(20, rest);
    CHECK(r.events == 0, "rest: %d events", r.events);
    r = run(12, scratching);
    CHECK(r.events == 1, "nail scratching: %d events", r.events);
    CHECK(fabsf(r.first_t - 3) <= 1.5f, "nail scratching starts at %.1f s, want ~3", r.first_t);
    CHECK(r.first_dur >= 4 && r.first_dur <= 8, "nail scratching lasts %.1f s, want ~6", r.first_dur);
    r = run(20, long_scratching);
    CHECK(r.events == 1 && r.first_dur >= 16, "long scratching: %d events, first %.1f s", r.events, r.first_dur);
    r = run(12, finger_scratching);
    CHECK(r.events == 1, "finger-only scratching: %d events (the old 100 Hz detector missed this)", r.events);
    r = run(20, waving);
    CHECK(r.events == 0 && r.scratchy_windows == 0, "waving: %d events, %d scratchy s (the old detector flagged this)",
          r.events, r.scratchy_windows);
    r = run(30, walking);
    CHECK(r.events == 0, "walking: %d events, %d scratchy s", r.events, r.scratchy_windows);
    r = run(30, typing);
    CHECK(r.events == 0, "typing: %d events, %d scratchy s", r.events, r.scratchy_windows);
    r = run(10, jolt);
    CHECK(r.events == 0, "jolt: %d events", r.events);
}

// --- skin conductance and temperature ---------------------------------------------------
static void test_skin(void) {
    puts("skin sensors");
    float c = skin_ntc_c(1650, 3300, 10000, 10000, 3950);
    CHECK(fabsf(c - 25.0f) < 0.01f, "NTC at half the supply is its rated 25 C, got %.2f", c);
    c = skin_ntc_c(1236.13f, 3300, 10000, 10000, 3950);  // worked out from the beta equation
    CHECK(fabsf(c - 37.0f) < 0.05f, "NTC at 37 C, got %.2f", c);
    CHECK(!skin_temp_plausible(skin_ntc_c(3300, 3300, 10000, 10000, 3950)), "open thermistor (pin at 3V3) rejected");
    CHECK(!skin_temp_plausible(skin_ntc_c(0, 3300, 10000, 10000, 3950)), "shorted thermistor (pin at 0 V) rejected");
    CHECK(!skin_temp_plausible(skin_ntc_c(3100, 3300, 10000, 10000, 3950)), "a pin near 3V3 (no NTC) is out of range");
    float us = skin_gsr_us(967.74f, 3300, 512);
    CHECK(fabsf(us - 13.054f) < 0.01f, "Grove a=300 is 13.05 uS, got %.3f", us);
    CHECK(skin_gsr_us(1651.6f, 3300, 512) == 0, "Grove at its no-skin reading means not on skin");
    CHECK(!skin_gsr_plausible(0), "0 uS is not sent");
    // the simulator: calm stays plausible, stress raises conductance
    skin_sim_t sim;
    skin_sim_init(&sim, 7);
    float calm = 0, stressed = 0, t;
    int bad = 0;
    for (int i = 0; i < 4 * 600; i++) {
        skin_sim_step(&sim, &us, &t);
        bad += !skin_gsr_plausible(us) || !skin_temp_plausible(t);
        if (i >= 4 * 300) calm += us / (4 * 300);
    }
    sim.stressed = true;
    for (int i = 0; i < 4 * 600; i++) {
        skin_sim_step(&sim, &us, &t);
        bad += !skin_gsr_plausible(us) || !skin_temp_plausible(t);
        if (i >= 4 * 300) stressed += us / (4 * 300);
    }
    CHECK(bad == 0, "%d simulated readings out of range", bad);
    CHECK(stressed > calm * 1.5f, "simulated stress raises conductance: %.2f vs calm %.2f uS", stressed, calm);
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

// --- screens and warnings ---------------------------------------------------------------
static void test_ui_flow(void) {
    puts("screens and warnings");
    ui_flow_t f;
    ui_flow_init(&f);
    ui_inputs_t in = {.state = ST_BOOT, .outbox_cap = 3600, .gsr = SENSOR_ABSENT, .temp = SENSOR_ABSENT};
    CHECK(ui_flow_screen(&f, &in) == SCR_BOOT, "boot");
    in.state = ST_PAIRING, in.now_ms = 1000;
    CHECK(ui_flow_screen(&f, &in) == SCR_CONNECT, "pairing shows Connect");
    in.state = ST_READY, in.linked = true;
    CHECK(ui_flow_screen(&f, &in) == SCR_HOME, "ready shows Home");
    in.held_ms = 100;
    CHECK(ui_flow_screen(&f, &in) == SCR_HOME, "a tap-length touch doesn't flash the hold screen");
    in.held_ms = 900;
    CHECK(ui_flow_screen(&f, &in) == SCR_HOLD_START, "holding shows Hold to start");
    CHECK(ui_flow_hold_progress(&in) > 0.4f && ui_flow_hold_progress(&in) < 0.5f, "the ring is under half full at 0.9 s");
    in.held_ms = 0;
    CHECK(ui_flow_screen(&f, &in) == SCR_HOME, "letting go early goes back to Home");

    in.state = ST_RECORDING, in.now_ms = 5000, in.gsr = SENSOR_OK;
    CHECK(ui_flow_screen(&f, &in) == SCR_STARTED, "a new recording shows Started");
    in.now_ms = 5000 + UI_STARTED_MS + 1;
    CHECK(ui_flow_screen(&f, &in) == SCR_RECORDING, "then Recording");
    CHECK(ui_flow_warning(&f, &in) == WARN_NONE, "no warning while all is well");
    in.page = 1;
    CHECK(ui_flow_screen(&f, &in) == SCR_SENSORS, "a tap shows Sensors");
    in.page = 0;
    in.held_ms = 1200;
    CHECK(ui_flow_screen(&f, &in) == SCR_HOLD_STOP, "holding while recording shows Hold to stop");
    in.held_ms = 0;

    // the skin sensor comes off: a warning only after 10 s
    in.gsr = SENSOR_IDLE, in.now_ms = 20000;
    ui_flow_screen(&f, &in);
    CHECK(ui_flow_warning(&f, &in) == WARN_NONE, "no warning in the first seconds off the skin");
    in.now_ms = 20000 + UI_SENSOR_GRACE_MS;
    ui_flow_screen(&f, &in);
    CHECK(ui_flow_warning(&f, &in) == WARN_GSR_OFF, "skin sensor warning after 10 s");
    in.gsr = SENSOR_OK, in.now_ms += 100;
    ui_flow_screen(&f, &in);
    CHECK(ui_flow_warning(&f, &in) == WARN_NONE, "back on the skin clears it");
    in.temp = SENSOR_ABSENT;
    ui_flow_screen(&f, &in);
    CHECK(ui_flow_warning(&f, &in) == WARN_NONE, "a temperature sensor that never worked never warns");
    in.linked = false;
    CHECK(ui_flow_warning(&f, &in) == WARN_OFFLINE, "recording while not connected warns");
    in.outbox = 3000;
    CHECK(ui_flow_warning(&f, &in) == WARN_STORAGE, "memory nearly full outranks everything");
    in.outbox = 0, in.linked = true;

    in.state = ST_SYNCING, in.now_ms = 60000;
    CHECK(ui_flow_screen(&f, &in) == SCR_SAVED, "stopping shows Saved");
    in.now_ms = 60000 + UI_SAVED_MS + 1;
    CHECK(ui_flow_screen(&f, &in) == SCR_SYNCING, "then Syncing");
    CHECK(ui_flow_warning(&f, &in) == WARN_NONE, "skin warnings are for recordings only");
    in.state = ST_ERROR;
    CHECK(ui_flow_screen(&f, &in) == SCR_SENSOR_ERROR, "no accelerometer: the error screen");
}

int main(void) {
    srand(1);
    test_scratch();
    test_skin();
    test_ui_flow();
    test_touch();
    test_sm();
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures != 0;
}
