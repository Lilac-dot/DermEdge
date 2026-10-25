#include "scratch.h"

#include <math.h>
#include <string.h>

#define GRAVITY_ALPHA 0.02f  // ~0.3 Hz low-pass: tracks the wrist's slow tilt, not the strokes

void scratch_reset(scratch_t *s) {
    memset(s, 0, sizeof(*s));
}

static void analyse(scratch_t *s, scratch_motion_t *m) {
    // the window in time order, and the axis that moves the most
    static float w[SCRATCH_WIN][3];
    for (int i = 0; i < SCRATCH_WIN; i++)
        memcpy(w[i], s->buf[(s->head + i) % SCRATCH_WIN], sizeof(w[i]));
    float mean[3] = {0}, var[3] = {0}, sq = 0;
    for (int i = 0; i < SCRATCH_WIN; i++)
        for (int k = 0; k < 3; k++) mean[k] += w[i][k] / SCRATCH_WIN;
    for (int i = 0; i < SCRATCH_WIN; i++)
        for (int k = 0; k < 3; k++) {
            float d = w[i][k] - mean[k];
            var[k] += d * d;
            sq += w[i][k] * w[i][k];
        }
    int ax = var[0] >= var[1] && var[0] >= var[2] ? 0 : var[1] >= var[2] ? 1 : 2;
    float rms = sqrtf(sq / SCRATCH_WIN);

    // Normalised autocorrelation of that axis, one lag past each end of the scratch range so a
    // peak can be told from a slope: a slower motion (walking) only slopes through the range.
    float r[SCRATCH_LAG_MAX + 2] = {0};
    if (var[ax] > 1e-9f) {
        for (int lag = SCRATCH_LAG_MIN - 1; lag <= SCRATCH_LAG_MAX + 1; lag++) {
            for (int i = 0; i + lag < SCRATCH_WIN; i++)
                r[lag] += (w[i][ax] - mean[ax]) * (w[i + lag][ax] - mean[ax]);
            r[lag] /= var[ax];
        }
    }
    // The strongest peak, then the shortest-lag peak nearly as strong: a 5 Hz stroke also
    // peaks at twice its period, and the shortest one is the real stroke rate.
    float best = 0;
    int best_lag = 0;
    for (int lag = SCRATCH_LAG_MIN; lag <= SCRATCH_LAG_MAX; lag++)
        if (r[lag] >= r[lag - 1] && r[lag] >= r[lag + 1] && r[lag] > best) best = r[lag], best_lag = lag;
    for (int lag = SCRATCH_LAG_MIN; lag < best_lag; lag++)
        if (r[lag] >= r[lag - 1] && r[lag] >= r[lag + 1] && r[lag] >= 0.85f * best) {
            best = r[lag], best_lag = lag;
            break;
        }
    bool rhythmic = best >= SCRATCH_PERIODIC;

    m->t = (float)s->n / SCRATCH_FS;
    m->activity = rms;
    m->freq_hz = rhythmic ? (float)SCRATCH_FS / best_lag : 0;
    m->scratch_energy = rhythmic ? rms * best : 0;
    m->scratchy = rhythmic && rms >= SCRATCH_RMS_MIN && rms <= SCRATCH_RMS_MAX;
}

static void close_episode(scratch_t *s, scratch_event_t *e) {
    e->t = s->ep_start;
    e->duration_s = s->ep_last - s->ep_start;
    e->intensity = s->ep_windows ? s->ep_sum / s->ep_windows : 0;
    s->in_episode = false;
    s->ep_windows = 0;
    s->ep_sum = 0;
    s->episodes++;
}

int scratch_feed(scratch_t *s, float x, float y, float z, scratch_motion_t *motion,
                 scratch_event_t *event) {
    float a[3] = {x, y, z};
    if (s->n == 0) memcpy(s->g, a, sizeof(a));  // start the gravity estimate where the wrist is
    for (int k = 0; k < 3; k++) {
        s->g[k] += GRAVITY_ALPHA * (a[k] - s->g[k]);
        s->buf[s->head][k] = a[k] - s->g[k];
    }
    s->head = (s->head + 1) % SCRATCH_WIN;
    s->n++;
    if (s->filled < SCRATCH_WIN) s->filled++;
    if (++s->since_hop < SCRATCH_HOP || s->filled < SCRATCH_WIN) return 0;
    s->since_hop = 0;

    int got = SCRATCH_GOT_MOTION;
    analyse(s, motion);
    float win_start = motion->t - (float)SCRATCH_WIN / SCRATCH_FS;
    if (motion->scratchy) {
        s->run_off = 0;
        if (!s->in_episode && ++s->run_on >= SCRATCH_START_WINDOWS) {
            s->in_episode = true;
            // the first scratchy window began one hop per extra window before this one
            s->ep_start = win_start - (float)(SCRATCH_START_WINDOWS - 1) * SCRATCH_HOP / SCRATCH_FS;
        }
        if (s->in_episode) {
            s->ep_last = motion->t;
            s->ep_sum += motion->activity;
            s->ep_windows++;
        }
    } else {
        s->run_on = 0;
        if (s->in_episode && ++s->run_off >= SCRATCH_END_WINDOWS) {
            close_episode(s, event);
            s->run_off = 0;
            got |= SCRATCH_GOT_EVENT;
        }
    }
    return got;
}

bool scratch_flush(scratch_t *s, scratch_event_t *event) {
    if (!s->in_episode) return false;
    close_episode(s, event);
    return true;
}
