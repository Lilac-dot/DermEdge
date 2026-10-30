// Host tests for the band's plain-C logic:  sh test/run.sh
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../main/app_sm.h"
#include "../main/scratch.h"
#include "../main/skin.h"
#include "../main/store.h"
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

// --- heart (ECG) and skin temperature ------------------------------------------------------
// Runs `secs` seconds of the simulator through the detector and 30 s windows, as main.c does
// with the hand still; returns the windows' readings averaged, and how many there were
static int run_ecg(skin_sim_t *sim, ecg_det_t *d, int secs, float noise_mv, hrv_t *avg, float *live_bpm) {
    hrv_win_t w;
    hrv_win_reset(&w);
    memset(avg, 0, sizeof(*avg));
    int windows = 0;
    for (int s = 0; s < secs; s++) {
        for (int i = 0; i < SKIN_ECG_HZ; i++) {
            int rr;
            bool valid;
            if (ecg_det_feed(d, skin_sim_ecg(sim) + noise(noise_mv), true, &rr, &valid)) hrv_win_add_rr(&w, rr, valid);
        }
        hrv_win_add_second(&w, true);
        if (hrv_win_full(&w)) {
            hrv_t h;
            if (hrv_win_result(&w, &h)) {
                avg->hr += h.hr, avg->rmssd += h.rmssd, avg->sdnn += h.sdnn, avg->beats += h.beats;
                windows++;
            }
            hrv_win_reset(&w);
        }
    }
    if (windows) avg->hr /= windows, avg->rmssd /= windows, avg->sdnn /= windows, avg->beats /= windows;
    *live_bpm = ecg_det_bpm(d);
    return windows;
}

static void test_skin(void) {
    puts("heart and skin temperature");
    CHECK(fabsf(skin_lm35_c(330) - 33.0f) < 0.001f, "LM35: 330 mV is 33 C");
    CHECK(!skin_temp_plausible(skin_lm35_c(0)), "LM35 at 0 V (unplugged or shorted) rejected");
    CHECK(!skin_temp_plausible(skin_lm35_c(950)), "LM35 at the top of the ADC's range rejected");

    skin_sim_t sim;
    ecg_det_t d;
    hrv_t calm, stressed;
    float bpm;
    skin_sim_init(&sim, 7);
    ecg_det_init(&d);
    int n = run_ecg(&sim, &d, 120, 0, &calm, &bpm);
    CHECK(n == 4, "calm: every 30 s window gives a reading (%d of 4)", n);
    CHECK(fabsf(calm.hr - 68.2f) < 3, "calm heart rate about 68 bpm, got %.1f (T waves not counted as beats)", calm.hr);
    CHECK(fabsf(bpm - calm.hr) < 6, "live heart rate %.1f close to the window's %.1f", bpm, calm.hr);
    sim.stressed = true;
    run_ecg(&sim, &d, 30, 0, &stressed, &bpm);  // settle into the new rate
    n = run_ecg(&sim, &d, 120, 0, &stressed, &bpm);
    CHECK(n >= 3, "stressed: windows give readings (%d of 4)", n);
    CHECK(fabsf(stressed.hr - 92.3f) < 3, "stressed heart rate about 92 bpm, got %.1f", stressed.hr);
    CHECK(stressed.rmssd < calm.rmssd * 0.6f, "stress lowers RMSSD: %.1f vs calm %.1f ms", stressed.rmssd, calm.rmssd);
    CHECK(stressed.sdnn < calm.sdnn, "and SDNN: %.1f vs %.1f ms", stressed.sdnn, calm.sdnn);

    // noisy electrodes (forearm muscles, a loose pad) still find the beats
    sim.stressed = false;
    n = run_ecg(&sim, &d, 60, 0, &calm, &bpm);
    hrv_t noisy;
    n = run_ecg(&sim, &d, 60, 120, &noisy, &bpm);
    CHECK(n >= 1 && fabsf(noisy.hr - 68.2f) < 4, "120 mV of noise: %d windows, %.1f bpm", n, noisy.hr);

    // electrodes off resets everything; the first beat back has no interval
    int rr;
    bool valid;
    CHECK(!ecg_det_feed(&d, 1500, false, &rr, &valid) && ecg_det_bpm(&d) == 0, "electrodes off: no beats, no heart rate");

    // a window where the hand moved, or with too many artifacts, gives no reading
    hrv_win_t w;
    hrv_win_reset(&w);
    for (int i = 0; i < 34; i++) hrv_win_add_rr(&w, 880 + (i % 2) * 40, true);
    for (int s = 0; s < 30; s++) hrv_win_add_second(&w, s >= 5);  // moving for 5 s
    hrv_t h;
    CHECK(!hrv_win_result(&w, &h), "5 s of movement: no reading");
    hrv_win_reset(&w);
    for (int i = 0; i < 34; i++) hrv_win_add_rr(&w, 880, i % 4 != 0);  // every fourth beat bad
    for (int s = 0; s < 30; s++) hrv_win_add_second(&w, true);
    CHECK(!hrv_win_result(&w, &h), "25%% artifacts: no reading");
    hrv_win_reset(&w);
    int rrs[] = {800, 820, 790, 810, 800, 830, 790, 800, 810, 820, 800, 790, 810, 800, 820, 800};
    for (int i = 0; i < 16; i++) hrv_win_add_rr(&w, rrs[i], true);
    for (int s = 0; s < 30; s++) hrv_win_add_second(&w, true);
    CHECK(hrv_win_result(&w, &h), "a clean window gives a reading");
    // worked out by hand: mean 805.6 ms, successive differences give RMSSD 20.66 ms
    CHECK(fabsf(h.hr - 74.48f) < 0.05f && fabsf(h.rmssd - 20.66f) < 0.05f, "HR %.2f RMSSD %.2f", h.hr, h.rmssd);

    // the simulated temperature stays plausible and cools a little under stress
    skin_sim_init(&sim, 3);
    float c = 0;
    for (int i = 0; i < 600; i++) c = skin_sim_temp(&sim);
    sim.stressed = true;
    float cs = 0;
    for (int i = 0; i < 600; i++) cs = skin_sim_temp(&sim);
    CHECK(skin_temp_plausible(c) && skin_temp_plausible(cs) && cs < c, "simulated temp %.2f calm, %.2f stressed", c, cs);
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
    ui_inputs_t in = {.state = ST_BOOT, .outbox_cap = 3600, .temp = SENSOR_ABSENT};
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

    in.state = ST_RECORDING, in.now_ms = 5000, in.temp = SENSOR_OK;
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

    // the temperature sensor stops reading: a warning only after 10 s
    in.temp = SENSOR_ABSENT, in.now_ms = 20000;
    ui_flow_screen(&f, &in);
    CHECK(ui_flow_warning(&f, &in) == WARN_NONE, "no warning in the first seconds");
    in.now_ms = 20000 + UI_SENSOR_GRACE_MS;
    ui_flow_screen(&f, &in);
    CHECK(ui_flow_warning(&f, &in) == WARN_TEMP_OFF, "temperature warning after 10 s");
    in.temp = SENSOR_OK, in.now_ms += 100;
    ui_flow_screen(&f, &in);
    CHECK(ui_flow_warning(&f, &in) == WARN_NONE, "reading again clears it");
    ui_flow_t fresh;
    ui_flow_init(&fresh);
    ui_inputs_t never = in;
    never.temp = SENSOR_ABSENT;
    ui_flow_screen(&fresh, &never);
    never.now_ms += UI_SENSOR_GRACE_MS * 2;
    ui_flow_screen(&fresh, &never);
    CHECK(ui_flow_warning(&fresh, &never) != WARN_TEMP_OFF, "a temperature sensor that never worked never warns");
    in.linked = false;
    CHECK(ui_flow_warning(&f, &in) == WARN_OFFLINE, "recording while not connected warns");
    in.outbox = 3000;
    CHECK(ui_flow_warning(&f, &in) == WARN_STORAGE, "memory nearly full outranks everything");
    in.outbox = 0, in.linked = true;

    in.state = ST_SYNCING, in.now_ms = 60000;
    CHECK(ui_flow_screen(&f, &in) == SCR_SAVED, "stopping shows Saved");
    in.now_ms = 60000 + UI_SAVED_MS + 1;
    CHECK(ui_flow_screen(&f, &in) == SCR_SYNCING, "then Syncing");
    CHECK(ui_flow_warning(&f, &in) == WARN_NONE, "sensor warnings are for recordings only");
    in.state = ST_ERROR;
    CHECK(ui_flow_screen(&f, &in) == SCR_SENSOR_ERROR, "no accelerometer: the error screen");
}

// --- the flash outbox ------------------------------------------------------------------
// A simulated NOR flash: writes can only clear bits, an erase sets a whole sector to 0xFF
#define SIM_SECTORS 8
static uint8_t sim_mem[SIM_SECTORS * STORE_SECTOR];
static int sim_erases, sim_write_limit = -1;  // stop writing after this many bytes: power lost
static int sim_read(void *c, uint32_t off, void *buf, uint32_t len) { (void)c; memcpy(buf, sim_mem + off, len); return 0; }
static int sim_write(void *c, uint32_t off, const void *buf, uint32_t len) {
    (void)c;
    for (uint32_t i = 0; i < len; i++) {
        if (sim_write_limit == 0) return -1;
        if (sim_write_limit > 0) sim_write_limit--;
        sim_mem[off + i] &= ((const uint8_t *)buf)[i];
    }
    return 0;
}
static int sim_erase(void *c, uint32_t off) { (void)c; memset(sim_mem + off, 0xFF, STORE_SECTOR); sim_erases++; return 0; }
static store_flash_t sim_flash(void) { return (store_flash_t){sim_read, sim_write, sim_erase, NULL, sizeof(sim_mem)}; }

// a recognisable item: number n in bytes 4-7, byte 2 a kind (top bit left free)
static void mk(uint8_t *b, uint32_t n) { memset(b, 0, STORE_ITEM); b[2] = (uint8_t)(n % 4); memcpy(b + 4, &n, 4); b[15] = (uint8_t)n; }
static uint32_t num(const uint8_t *b) { uint32_t n; memcpy(&n, b + 4, 4); return n; }

static void test_store(void) {
    puts("flash outbox");
    memset(sim_mem, 0xFF, sizeof(sim_mem));  // a blank chip
    store_t st;
    uint8_t b[STORE_ITEM];
    CHECK(store_open(&st, sim_flash()) == 0 && store_count(&st) == 0, "opens a blank chip");
    uint32_t cap = store_capacity(&st);
    CHECK(cap == (SIM_SECTORS - 1) * STORE_PER_SECTOR, "capacity %u", (unsigned)cap);

    for (uint32_t n = 0; n < 600; n++) mk(b, n), store_append(&st, b, false);
    int bad = 0;
    for (uint32_t n = 0; n < 250; n++) {
        bad += !store_peek(&st, b) || num(b) != n || b[2] != n % 4 || b[15] != (uint8_t)n;
        store_pop(&st);
    }
    CHECK(bad == 0, "first in, first out, bytes intact (%d wrong)", bad);

    // a restart: open the same chip again
    CHECK(store_open(&st, sim_flash()) == 0 && store_count(&st) == 350, "after a restart, %u unsent (want 350)", (unsigned)store_count(&st));
    CHECK(store_peek(&st, b) && num(b) == 250, "and it carries on from item %u (want 250)", (unsigned)num(b));
    CHECK(store_peek_nth(&st, 3, b) && num(b) == 253 && !store_peek_nth(&st, 350, b), "peek_nth looks ahead without sending");

    // keep going round the ring many times: nothing lost, nothing repeated
    uint32_t next_in = 600, next_out = 250;
    sim_erases = 0;
    for (int i = 0; i < 10 * (int)cap; i++) {
        mk(b, next_in++);
        if (!store_append(&st, b, false)) { bad++; break; }
        if (store_count(&st) > cap / 2) {
            store_peek(&st, b);
            bad += num(b) != next_out++;
            store_pop(&st);
        }
    }
    CHECK(bad == 0, "ten times round the ring, every item once and in order");
    CHECK(sim_erases <= 10 * SIM_SECTORS + 2, "each sector erased about once per lap (%d erases)", sim_erases);

    // full: appends fail rather than lose unsent data, unless told to overwrite the oldest
    while (store_append(&st, (mk(b, next_in), b), false)) next_in++;
    uint32_t full = store_count(&st);
    CHECK(full >= cap, "full at %u items (capacity %u)", (unsigned)full, (unsigned)cap);
    store_peek(&st, b);
    uint32_t oldest = num(b);
    CHECK(store_append(&st, (mk(b, next_in), b), true), "an overwrite makes room");
    store_peek(&st, b);
    CHECK(num(b) > oldest && store_count(&st) < full, "by dropping the oldest sector (oldest now %u, was %u)", (unsigned)num(b), (unsigned)oldest);

    // power lost halfway through writing an item: the half-item is skipped, nothing blends
    memset(sim_mem, 0xFF, sizeof(sim_mem));
    store_open(&st, sim_flash());
    for (uint32_t n = 0; n < 10; n++) mk(b, n), store_append(&st, b, false);
    sim_write_limit = 7;  // the next item gets 7 of its 16 bytes
    mk(b, 999), store_append(&st, b, false);
    sim_write_limit = -1;
    store_open(&st, sim_flash());  // the restart after the power cut
    for (uint32_t n = 10; n < 15; n++) mk(b, n), store_append(&st, b, false);
    bad = 0;
    for (uint32_t n = 0; n < 15; n++) {
        if (!store_peek(&st, b)) { bad++; break; }
        bad += num(b) != n;
        store_pop(&st);
    }
    CHECK(bad == 0 && store_count(&st) == 0, "a half-written item after a power cut is skipped (%d wrong)", bad);

    // the rule for a filling outbox: older data kept, new data thinned
    int kept_motion = 0, kept_other = 0;
    for (int t = 0; t < 100; t++) kept_motion += store_should_keep(0.5f, false, true, t);
    CHECK(kept_motion == 100, "below 90%%: every motion summary (%d/100)", kept_motion);
    kept_motion = 0;
    for (int t = 0; t < 100; t++) kept_motion += store_should_keep(0.92f, false, true, t), kept_other += store_should_keep(0.92f, false, false, t);
    CHECK(kept_motion == 10 && kept_other == 100, "from 90%%: motion every 10 s (%d/100), heart and temperature all (%d/100)", kept_motion, kept_other);
    CHECK(!store_should_keep(0.99f, false, false, 0) && !store_should_keep(0.99f, false, true, 0), "from 98%%: no heart, temperature or motion");
    CHECK(store_should_keep(0.99f, true, false, 3) && store_should_keep(1.0f, true, false, 3), "scratch episodes always");
}

int main(void) {
    srand(1);
    test_scratch();
    test_skin();
    test_ui_flow();
    test_store();
    test_touch();
    test_sm();
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures != 0;
}
