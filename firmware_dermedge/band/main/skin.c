#include "skin.h"

#include <math.h>
#include <string.h>

float skin_lm35_c(float mv) { return mv / 10.0f; }

bool skin_temp_plausible(float c) { return c >= SKIN_TEMP_MIN_C && c <= SKIN_TEMP_MAX_C; }

bool skin_temp_offset(float measured, float reference, float *offset) {
    float d = reference - measured;
    if (!skin_temp_plausible(measured) || !skin_temp_plausible(reference) || fabsf(d) > SKIN_TEMP_MAX_OFFSET_C) return false;
    *offset = d;
    return true;
}

// --- R-peak detector -----------------------------------------------------------------------
#define MS_PER_SAMPLE (1000 / SKIN_ECG_HZ)

void ecg_det_init(ecg_det_t *d) { memset(d, 0, sizeof(*d)); }

static int median(const uint16_t *v, int n) {
    uint16_t s[5];
    memcpy(s, v, n * sizeof(*v));
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && s[j - 1] > s[j]; j--) {
            uint16_t t = s[j]; s[j] = s[j - 1]; s[j - 1] = t;
        }
    return s[n / 2];
}

static void remember(ecg_det_t *d, int rr) {
    if (d->n_recent < 5) d->recent[d->n_recent++] = rr;
    else memmove(d->recent, d->recent + 1, 4 * sizeof(*d->recent)), d->recent[4] = rr;
}

bool ecg_det_feed(ecg_det_t *d, float mv, bool leads_on, int *rr_ms, bool *valid) {
    if (!leads_on) {
        ecg_det_init(d);
        return false;
    }
    d->n++;
    if (!d->primed) d->baseline = mv, d->primed = true;
    d->baseline += (mv - d->baseline) / 64.0f;  // about 0.25 s
    float dev = fabsf(mv - d->baseline);
    d->envelope = fmaxf(d->envelope * 0.999f, dev);  // loses 22% a second: T waves stay below 60%
    bool now = dev > ECG_MIN_PEAK_MV && dev > 0.6f * d->envelope;
    bool rising = now && !d->above;
    d->above = now;
    if (!rising) return false;
    uint32_t since = d->last_beat ? (d->n - d->last_beat) * MS_PER_SAMPLE : 0;
    if (d->last_beat && since < ECG_REFRACTORY_MS) return false;
    d->last_beat = d->n;
    *rr_ms = (int)since;
    *valid = false;
    if (since >= ECG_RR_MIN_MS && since <= ECG_RR_MAX_MS) {
        if (d->n_recent < 3) *valid = true;  // too few to judge yet: trust it
        else {
            int m = median(d->recent, d->n_recent);
            *valid = fabsf((float)since - m) <= ECG_RR_JUMP * m;
        }
        // remember plausible intervals even if they jumped, so a real change in heart rate is
        // accepted after a few beats instead of being refused forever
        remember(d, since);
    }
    return true;
}

float ecg_det_bpm(const ecg_det_t *d) {
    if (d->n_recent < 3) return 0;
    if ((d->n - d->last_beat) * MS_PER_SAMPLE > 3000) return 0;  // no beat for 3 s
    return 60000.0f / median(d->recent, d->n_recent);
}

// --- 30 s HRV windows -----------------------------------------------------------------------
void hrv_win_reset(hrv_win_t *w) { memset(w, 0, sizeof(*w)); }

void hrv_win_add_rr(hrv_win_t *w, int rr_ms, bool valid) {
    if (rr_ms <= 0) return;  // the first beat has no interval
    if (!valid) w->n_bad++;
    if (w->n_rr < HRV_MAX_RR) w->rr[w->n_rr++] = valid ? (uint16_t)rr_ms : 0;
}

void hrv_win_add_second(hrv_win_t *w, bool still) {
    w->secs++;
    w->secs_still += still;
}

bool hrv_win_full(const hrv_win_t *w) { return w->secs >= HRV_WINDOW_S; }

bool hrv_win_result(const hrv_win_t *w, hrv_t *out) {
    int good = 0;
    double sum = 0, sq = 0, dsq = 0;
    int pairs = 0;
    for (int i = 0; i < w->n_rr; i++) {
        if (!w->rr[i]) continue;
        good++;
        sum += w->rr[i];
        if (i && w->rr[i - 1]) {  // successive differences only between neighbouring good beats
            double dd = (double)w->rr[i] - w->rr[i - 1];
            dsq += dd * dd;
            pairs++;
        }
    }
    int total = good + w->n_bad;
    if (good < HRV_MIN_BEATS || pairs < 2 || (total && (float)w->n_bad / total > HRV_MAX_BAD) ||
        w->secs_still < HRV_MIN_STILL_S)
        return false;
    double mean = sum / good;
    for (int i = 0; i < w->n_rr; i++)
        if (w->rr[i]) sq += (w->rr[i] - mean) * (w->rr[i] - mean);
    out->hr = (float)(60000.0 / mean);
    out->rmssd = (float)sqrt(dsq / pairs);
    out->sdnn = (float)sqrt(sq / good);
    out->beats = good;
    return true;
}

// --- simulator ------------------------------------------------------------------------------
static float rnd(skin_sim_t *s) {  // 0..1, xorshift: the same run on the band and the Mac
    s->seed ^= s->seed << 13;
    s->seed ^= s->seed >> 17;
    s->seed ^= s->seed << 5;
    return (s->seed & 0xFFFFFF) / (float)0xFFFFFF;
}

static float gauss(float x, float mu, float sd) { return expf(-0.5f * (x - mu) * (x - mu) / (sd * sd)); }

void skin_sim_init(skin_sim_t *s, uint32_t seed) {
    memset(s, 0, sizeof(*s));
    s->seed = seed ? seed : 1;
    s->rr_ms = 880;
    s->temp = 32.0f;
}

float skin_sim_ecg(skin_sim_t *s) {
    const float dt = 1000.0f / SKIN_ECG_HZ;
    s->t_s += dt / 1000;
    s->since_ms += dt;
    if (s->since_ms >= s->rr_ms) {  // next beat: breathing (every ~4 s) swings the interval when calm
        s->since_ms -= s->rr_ms;
        float mean = s->stressed ? 650 : 880, swing = s->stressed ? 12 : 55, jitter = s->stressed ? 6 : 18;
        s->rr_ms = mean + swing * sinf(2 * 3.14159f * s->t_s / 4.0f) + (rnd(s) - 0.5f) * 2 * jitter;
    }
    float x = s->since_ms;
    float v = 650 * gauss(x, 40, 10)      // R
            - 90 * gauss(x, 60, 8)        // S
            + 170 * gauss(x, 300, 45);    // T
    return 1500 + v + 60 * sinf(2 * 3.14159f * s->t_s / 7.0f) + (rnd(s) - 0.5f) * 30;
}

float skin_sim_temp(skin_sim_t *s) {
    s->temp += ((s->stressed ? 31.4f : 32.0f) - s->temp) / 60 + (rnd(s) - 0.5f) * 0.02f;
    return s->temp;
}
