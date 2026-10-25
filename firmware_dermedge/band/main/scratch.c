#include "scratch.h"

#include <math.h>
#include <string.h>

#include "scratch_model.h"

// a trained model brings its own episode ending, tuned with it
#if SCRATCH_MODEL_TRAINED && defined(SCRATCH_MODEL_END_WINDOWS)
#define END_WINDOWS SCRATCH_MODEL_END_WINDOWS
#else
#define END_WINDOWS SCRATCH_END_WINDOWS
#endif

#define PI_F 3.14159265f
#define GRAVITY_ALPHA 0.02f  // ~0.3 Hz low-pass at 100 Hz: tracks the hand's slow tilt, not the strokes
#define HP_HZ 80.0f          // vibration starts here

static const float BAND_EDGES[SCRATCH_N_BANDS + 1] = {2, 5, 20, 50, 80, 140, 250, 400};
#define VIB_FIRST_BAND 4  // 80-140, 140-250, 250-400 Hz

static struct { float b0, b1, b2, a1, a2; } hp;  // RBJ high-pass, Butterworth Q
static float hann[SCRATCH_HF_WIN], hann_power;
static bool tables_ready;

static void make_tables(void) {
    float w0 = 2 * PI_F * HP_HZ / SCRATCH_FS, c = cosf(w0), alpha = sinf(w0) / (2 * 0.70710678f), a0 = 1 + alpha;
    hp.b0 = (1 + c) / 2 / a0;
    hp.b1 = -(1 + c) / a0;
    hp.b2 = (1 + c) / 2 / a0;
    hp.a1 = -2 * c / a0;
    hp.a2 = (1 - alpha) / a0;
    hann_power = 0;
    for (int i = 0; i < SCRATCH_HF_WIN; i++) {
        hann[i] = 0.5f - 0.5f * cosf(2 * PI_F * i / (SCRATCH_HF_WIN - 1));
        hann_power += hann[i] * hann[i];
    }
    tables_ready = true;
}

void scratch_reset(scratch_t *s) {
    memset(s, 0, sizeof(*s));
    if (!tables_ready) make_tables();
}

// In-place radix-2 FFT of SCRATCH_FFT points
static void fft(float *re, float *im) {
    for (int i = 1, j = 0; i < SCRATCH_FFT; i++) {
        int bit = SCRATCH_FFT >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= SCRATCH_FFT; len <<= 1) {
        float ang = -2 * PI_F / len, wr = cosf(ang), wi = sinf(ang);
        for (int i = 0; i < SCRATCH_FFT; i += len) {
            float cr = 1, ci = 0;
            for (int k = 0; k < len / 2; k++) {
                int a = i + k, b = i + k + len / 2;
                float tr = re[b] * cr - im[b] * ci, ti = re[b] * ci + im[b] * cr;
                re[b] = re[a] - tr; im[b] = im[a] - ti;
                re[a] += tr; im[a] += ti;
                float ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = ncr;
            }
        }
    }
}

// The strongest autocorrelation peak at the stroke-rate lags of a 100 Hz series, then the
// shortest-lag peak nearly as strong: a 5 Hz stroke also peaks at twice its period. Peaks, not
// the range's edges, so a slower motion (walking) only slopes through.
static float rhythm(const float *x, int n, int *lag_out) {
    float mean = 0, var = 0, r[SCRATCH_LAG_MAX + 2] = {0};
    for (int i = 0; i < n; i++) mean += x[i] / n;
    for (int i = 0; i < n; i++) var += (x[i] - mean) * (x[i] - mean);
    *lag_out = 0;
    if (var < 1e-12f) return 0;
    for (int lag = SCRATCH_LAG_MIN - 1; lag <= SCRATCH_LAG_MAX + 1; lag++) {
        for (int i = 0; i + lag < n; i++) r[lag] += (x[i] - mean) * (x[i + lag] - mean);
        r[lag] /= var;
    }
    float best = 0;
    int best_lag = 0;
    for (int lag = SCRATCH_LAG_MIN; lag <= SCRATCH_LAG_MAX; lag++)
        if (r[lag] >= r[lag - 1] && r[lag] >= r[lag + 1] && r[lag] > best) best = r[lag], best_lag = lag;
    for (int lag = SCRATCH_LAG_MIN; lag < best_lag; lag++)
        if (r[lag] >= r[lag - 1] && r[lag] >= r[lag + 1] && r[lag] >= 0.85f * best) {
            best = r[lag], best_lag = lag;
            break;
        }
    *lag_out = best_lag;
    return best;
}

static float score(const float *f, bool *scratchy) {
#if SCRATCH_MODEL_TRAINED
    float z = SCRATCH_MODEL_BIAS;
    for (int i = 0; i < SCRATCH_N_FEATURES; i++) z += SCRATCH_MODEL_W[i] * (f[i] - SCRATCH_MODEL_MEAN[i]) / SCRATCH_MODEL_SD[i];
    float p = 1 / (1 + expf(-z));
    *scratchy = p >= SCRATCH_MODEL_THRESHOLD && f[F_RMS] <= SCRATCH_RMS_MAX;
    return p;
#else
    float vib = 0;
    for (int b = VIB_FIRST_BAND; b < SCRATCH_N_BANDS; b++) vib += powf(10, f[F_BAND0 + b]);
    *scratchy = vib >= SCRATCH_RULE_VIB_MIN && (f[F_RHYTHM] >= SCRATCH_RULE_RHYTHM || f[F_VIB_RHYTHM] >= SCRATCH_RULE_RHYTHM) &&
                f[F_RMS] <= SCRATCH_RMS_MAX;
    return *scratchy ? 1 : 0;
#endif
}

static void analyse(scratch_t *s, scratch_motion_t *m) {
    // --- motion: 2 s at 100 Hz, as one series per axis in time order
    static float lf[3][SCRATCH_LF_WIN], series[SCRATCH_LF_WIN];
    float var[3] = {0}, sq = 0;
    for (int i = 0; i < SCRATCH_LF_WIN; i++)
        for (int k = 0; k < 3; k++) {
            lf[k][i] = s->lf[(s->lf_head + i) % SCRATCH_LF_WIN][k];
            sq += lf[k][i] * lf[k][i];
        }
    for (int k = 0; k < 3; k++) {
        float mean = 0;
        for (int i = 0; i < SCRATCH_LF_WIN; i++) mean += lf[k][i] / SCRATCH_LF_WIN;
        for (int i = 0; i < SCRATCH_LF_WIN; i++) var[k] += (lf[k][i] - mean) * (lf[k][i] - mean);
    }
    int ax = var[0] >= var[1] && var[0] >= var[2] ? 0 : var[1] >= var[2] ? 1 : 2;
    int lag, env_lag;
    float f[SCRATCH_N_FEATURES];
    f[F_RMS] = sqrtf(sq / SCRATCH_LF_WIN);
    f[F_RHYTHM] = rhythm(lf[ax], SCRATCH_LF_WIN, &lag);

    // --- vibration: band powers over 1 s at 800 Hz, summed over the three axes
    static float re[SCRATCH_FFT], im[SCRATCH_FFT];
    float band[SCRATCH_N_BANDS] = {0};
    for (int k = 0; k < 3; k++) {
        float mean = 0;
        for (int i = 0; i < SCRATCH_HF_WIN; i++) mean += s->hf[i][k] / SCRATCH_HF_WIN;
        memset(re, 0, sizeof(re));
        memset(im, 0, sizeof(im));
        for (int i = 0; i < SCRATCH_HF_WIN; i++) re[i] = (s->hf[(s->hf_head + i) % SCRATCH_HF_WIN][k] - mean) * hann[i];
        fft(re, im);
        // one-sided power, scaled so a band's sum is the signal's mean square in that band
        float scale = 2.0f / ((float)SCRATCH_FFT * hann_power);
        for (int b = 0; b < SCRATCH_N_BANDS; b++) {
            int lo = (int)ceilf(BAND_EDGES[b] * SCRATCH_FFT / SCRATCH_FS), hi = (int)ceilf(BAND_EDGES[b + 1] * SCRATCH_FFT / SCRATCH_FS);
            if (hi > SCRATCH_FFT / 2) hi = SCRATCH_FFT / 2;
            for (int i = lo; i < hi; i++) band[b] += (re[i] * re[i] + im[i] * im[i]) * scale;
        }
    }
    float vib = 0, total = 0;
    for (int b = 0; b < SCRATCH_N_BANDS; b++) {
        f[F_BAND0 + b] = log10f(band[b] + 1e-9f);
        total += band[b];
        if (b >= VIB_FIRST_BAND) vib += band[b];
    }
    for (int i = 0; i < SCRATCH_LF_WIN; i++) series[i] = s->env[(s->lf_head + i) % SCRATCH_LF_WIN];
    f[F_VIB_RHYTHM] = rhythm(series, SCRATCH_LF_WIN, &env_lag);
    f[F_VIB_RATIO] = total > 0 ? vib / total : 0;

    m->t = (float)s->n / SCRATCH_FS;
    m->activity = f[F_RMS];
    m->scratch_energy = sqrtf(vib);
    int stroke = f[F_RHYTHM] >= f[F_VIB_RHYTHM] ? lag : env_lag;
    m->freq_hz = stroke && fmaxf(f[F_RHYTHM], f[F_VIB_RHYTHM]) >= SCRATCH_RULE_RHYTHM ? (float)SCRATCH_LF_FS / stroke : 0;
    m->p = score(f, &m->scratchy);
    memcpy(m->f, f, sizeof(f));
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

int scratch_feed(scratch_t *s, float x, float y, float z, scratch_motion_t *motion, scratch_event_t *event) {
    float a[3] = {x, y, z};
    // vibration path, every sample: keep it for the spectrum, and high-pass it for the envelope
    for (int k = 0; k < 3; k++) {
        s->hf[s->hf_head][k] = a[k];
        float *st = s->hp_state[k];  // x1, x2, y1, y2
        float yk = hp.b0 * a[k] + hp.b1 * st[0] + hp.b2 * st[1] - hp.a1 * st[2] - hp.a2 * st[3];
        st[1] = st[0], st[0] = a[k], st[3] = st[2], st[2] = yk;
        s->env_acc += fabsf(yk);
        s->acc[k] += a[k];
    }
    s->hf_head = (s->hf_head + 1) % SCRATCH_HF_WIN;
    if (s->hf_filled < SCRATCH_HF_WIN) s->hf_filled++;
    s->n++;

    // motion path, every 8 samples: the block's mean, minus a slow gravity estimate
    if (++s->acc_n == SCRATCH_DECIM) {
        float m[3];
        for (int k = 0; k < 3; k++) m[k] = s->acc[k] / SCRATCH_DECIM, s->acc[k] = 0;
        if (s->n == SCRATCH_DECIM) memcpy(s->g, m, sizeof(m));  // start where the hand is
        for (int k = 0; k < 3; k++) {
            s->g[k] += GRAVITY_ALPHA * (m[k] - s->g[k]);
            s->lf[s->lf_head][k] = m[k] - s->g[k];
        }
        s->env[s->lf_head] = s->env_acc / SCRATCH_DECIM;
        s->env_acc = 0;
        s->acc_n = 0;
        s->lf_head = (s->lf_head + 1) % SCRATCH_LF_WIN;
        if (s->lf_filled < SCRATCH_LF_WIN) s->lf_filled++;
    }

    if (++s->since_hop < SCRATCH_HOP || s->lf_filled < SCRATCH_LF_WIN) return 0;
    s->since_hop = 0;

    int got = SCRATCH_GOT_MOTION;
    analyse(s, motion);
    float win_start = motion->t - (float)SCRATCH_LF_WIN / SCRATCH_LF_FS;
    if (motion->scratchy) {
        s->run_off = 0;
        if (!s->in_episode && ++s->run_on >= SCRATCH_START_WINDOWS) {
            s->in_episode = true;
            // the first scratchy window began one hop per extra window before this one
            s->ep_start = win_start - (float)(SCRATCH_START_WINDOWS - 1) * SCRATCH_HOP / SCRATCH_FS;
        }
        if (s->in_episode) {
            s->ep_last = motion->t;
            s->ep_sum += motion->scratch_energy;
            s->ep_windows++;
        }
    } else {
        s->run_on = 0;
        if (s->in_episode && ++s->run_off >= END_WINDOWS) {
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
