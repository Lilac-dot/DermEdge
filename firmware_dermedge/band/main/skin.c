#include "skin.h"

#include <math.h>

float skin_gsr_reading(float mv, float supply_mv) {
    return mv * 1023.0f / supply_mv;
}

float skin_gsr_us(float mv, float supply_mv, float no_skin) {
    float a = skin_gsr_reading(mv, supply_mv);
    if (a >= no_skin - 1) return 0;  // electrodes not on skin
    float ohms = (1024.0f + 2.0f * a) * 10000.0f / (no_skin - a);
    return 1e6f / ohms;
}

float skin_ntc_c(float mv, float supply_mv, float r_series, float r25, float beta) {
    if (mv <= 1 || mv >= supply_mv - 1) return -273.15f;  // open or shorted: out of range on purpose
    float r = r_series * mv / (supply_mv - mv);           // the thermistor's share of the divider
    float inv_t = 1.0f / 298.15f + logf(r / r25) / beta;
    return 1.0f / inv_t - 273.15f;
}

bool skin_gsr_plausible(float us) { return us >= SKIN_GSR_MIN_US && us <= SKIN_GSR_MAX_US; }
bool skin_temp_plausible(float c) { return c >= SKIN_TEMP_MIN_C && c <= SKIN_TEMP_MAX_C; }

// --- simulator ---------------------------------------------------------------------------
static float rnd(skin_sim_t *s) {  // 0..1, xorshift: the same run on the band and the Mac
    s->seed ^= s->seed << 13;
    s->seed ^= s->seed >> 17;
    s->seed ^= s->seed << 5;
    return (s->seed & 0xFFFFFF) / (float)0xFFFFFF;
}

void skin_sim_init(skin_sim_t *s, uint32_t seed) {
    s->level = 1.2f;   // microsiemens: a typical calm wrist
    s->scr = 0;
    s->temp = 32.0f;
    s->seed = seed ? seed : 1;
    s->stressed = false;
}

void skin_sim_step(skin_sim_t *s, float *us, float *c) {
    const float dt = 1.0f / SKIN_GSR_HZ, tau = 20.0f;  // physiology drifts over ~20 s
    float target = s->stressed ? 2.6f : 1.2f, rate = s->stressed ? 0.06f : 0.01f;
    s->level += (target - s->level) * dt / tau;
    if (rnd(s) < rate) s->scr += s->stressed ? 0.4f : 0.12f;  // a skin-conductance response
    s->scr *= 0.93f;
    *us = s->level + s->scr + (rnd(s) - 0.5f) * 0.02f;
    s->temp += ((s->stressed ? 31.4f : 32.0f) - s->temp) * dt / 60 + (rnd(s) - 0.5f) * 0.004f;
    *c = s->temp;
}
