#include "signals.h"

#include <math.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_random.h"

#if defined(CONFIG_DERMEDGE_GSR_GROVE) || defined(CONFIG_DERMEDGE_TEMP_LM35)
#define USE_ADC1
#include "esp_adc/adc_oneshot.h"
#endif
#ifdef CONFIG_DERMEDGE_TEMP_LM35
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#endif
#ifdef CONFIG_DERMEDGE_MOISTURE_TOUCH
#include "driver/touch_pad.h"
#endif

static const char *TAG = "signals";
static volatile bool s_stressed = false;

// Simulated levels, roughly those of WESAD participant S16 (the one the hub's simulator
// replays): skin conductance ~0.3 uS calm / ~1.7 uS during the stress task, wrist
// temperature ~29.4 / ~31.1 C, heart rate ~70 / ~95 bpm.
#define EDA_CALM 0.30f
#define EDA_STRESS 1.70f
#define TEMP_CALM 29.4f
#define TEMP_STRESS 31.1f
#define HR_CALM 70.0f
#define HR_STRESS 95.0f
#define TAU_S 20.0f  // physiology drifts toward the new state over ~20 s, not instantly

static float noise(float amp) { return amp * ((float)(esp_random() % 2001) / 1000.0f - 1.0f); }

// First-order approach toward the calm or stressed target, advanced once per call.
static float approach(float *level, float target, float dt)
{
    *level += (target - *level) * (dt / TAU_S);
    return *level;
}

#ifdef USE_ADC1
static adc_oneshot_unit_handle_t s_adc;
#endif
#ifdef CONFIG_DERMEDGE_TEMP_LM35
static adc_cali_handle_t s_temp_cali;
#define LM35_READS 32  // averaged per 4 Hz sample
#endif

void signals_init(void)
{
#ifdef USE_ADC1
    adc_oneshot_unit_init_cfg_t unit = {.unit_id = ADC_UNIT_1};
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit, &s_adc));
#endif
#ifdef CONFIG_DERMEDGE_GSR_GROVE
    adc_oneshot_chan_cfg_t ch = {.bitwidth = ADC_BITWIDTH_12, .atten = ADC_ATTEN_DB_12};
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, CONFIG_DERMEDGE_GSR_ADC_CHANNEL, &ch));
#endif
#ifdef CONFIG_DERMEDGE_TEMP_LM35
    // LM35 gives 10 mV per degree C, so skin (~30-35 C) is ~300-350 mV: 0 dB attenuation
    // (~100-950 mV) is the ESP32 ADC's most accurate range. Line fitting converts raw readings
    // to millivolts, using the chip's eFuse Vref or 1100 mV if none was burnt.
    adc_oneshot_chan_cfg_t tch = {.bitwidth = ADC_BITWIDTH_12, .atten = ADC_ATTEN_DB_0};
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, CONFIG_DERMEDGE_TEMP_ADC_CHANNEL, &tch));
    adc_cali_line_fitting_config_t cali = {.unit_id = ADC_UNIT_1, .atten = ADC_ATTEN_DB_0,
                                           .bitwidth = ADC_BITWIDTH_12, .default_vref = 1100};
    ESP_ERROR_CHECK(adc_cali_create_scheme_line_fitting(&cali, &s_temp_cali));
    float c = signals_temp(0);
    if (c < 20.0f || c > 42.0f) {
        ESP_LOGW(TAG, "LM35 reads %.1f C -- check the wiring (+Vs on 5V, not 3V3) and the ADC channel", c);
    } else {
        ESP_LOGI(TAG, "LM35 reads %.1f C", c);
    }
#endif
#ifdef CONFIG_DERMEDGE_MOISTURE_TOUCH
    ESP_ERROR_CHECK(touch_pad_init());
    touch_pad_set_fsm_mode(TOUCH_FSM_MODE_TIMER);
    touch_pad_set_voltage(TOUCH_HVOLT_2V7, TOUCH_LVOLT_0V5, TOUCH_HVOLT_ATTEN_1V);
    touch_pad_config(TOUCH_PAD_NUM0, 0);  // GPIO4: patch over the eczema-prone area
    touch_pad_config(TOUCH_PAD_NUM3, 0);  // GPIO15: reference patch on healthy skin
#endif
    ESP_LOGI(TAG, "GSR %s, skin temperature %s, moisture patch %s",
             signals_gsr_simulated() ? "simulated" : "Grove module",
             signals_temp_simulated() ? "simulated" : "LM35",
#if defined(CONFIG_DERMEDGE_MOISTURE_TOUCH)
             "touch electrodes"
#elif defined(CONFIG_DERMEDGE_MOISTURE_SIMULATED)
             "simulated"
#else
             "off"
#endif
    );
}

void signals_set_stressed(bool stressed) { s_stressed = stressed; }
bool signals_stressed(void) { return s_stressed; }

bool signals_gsr_simulated(void)
{
#ifdef CONFIG_DERMEDGE_GSR_GROVE
    return false;
#else
    return true;
#endif
}

bool signals_temp_simulated(void)
{
#ifdef CONFIG_DERMEDGE_TEMP_LM35
    return false;
#else
    return true;
#endif
}

float signals_eda(float t)
{
#ifdef CONFIG_DERMEDGE_GSR_GROVE
    (void)t;
    // Grove GSR: the module's own formula is R = (1024 + 2a) * 10000 / (512 - a) ohms for a
    // 10-bit reading a, where 512 is the reading with nothing touching the electrodes.
    // Scaled from the ESP32's 12-bit ADC; approximate -- check the no-skin reading of your
    // module and adjust GSR_NO_SKIN if it isn't near 512.
    const float GSR_NO_SKIN = 512.0f;
    int raw = 0;
    adc_oneshot_read(s_adc, CONFIG_DERMEDGE_GSR_ADC_CHANNEL, &raw);
    float a = raw * 1023.0f / 4095.0f;
    if (a >= GSR_NO_SKIN - 1) {
        return 0.0f;  // electrodes not on skin
    }
    float ohms = (1024.0f + 2.0f * a) * 10000.0f / (GSR_NO_SKIN - a);
    return 1e6f / ohms;  // microsiemens
#else
    static float level = EDA_CALM;
    static float scr = 0;  // skin-conductance responses: sudden rises that decay over seconds
    approach(&level, s_stressed ? EDA_STRESS : EDA_CALM, 0.25f);
    float rate = s_stressed ? 0.06f : 0.01f;  // responses per 4 Hz sample
    if ((esp_random() % 1000) < rate * 1000) {
        scr += s_stressed ? 0.25f : 0.08f;
    }
    scr *= 0.93f;
    (void)t;
    return level + scr + noise(0.01f);
#endif
}

float signals_temp(float t)
{
    (void)t;
#ifdef CONFIG_DERMEDGE_TEMP_LM35
    // One ESP32 ADC read is noisy by a few mV (several tenths of a degree), as much as the
    // ~1 C calm/stress shift the model looks for, so average a burst of reads.
    int sum = 0;
    for (int i = 0; i < LM35_READS; i++) {
        int raw = 0;
        adc_oneshot_read(s_adc, CONFIG_DERMEDGE_TEMP_ADC_CHANNEL, &raw);
        sum += raw;
    }
    int mv = 0;
    adc_cali_raw_to_voltage(s_temp_cali, sum / LM35_READS, &mv);
    return mv / 10.0f;  // 10 mV per degree C
#else
    // No skin-temperature sensor configured, so this channel is simulated.
    static float level = TEMP_CALM;
    return approach(&level, s_stressed ? TEMP_STRESS : TEMP_CALM, 0.25f) + noise(0.02f);
#endif
}

float signals_bvp(float t)
{
    // PPG-like pulse: a systolic peak plus a smaller dicrotic wave each beat. Phase is
    // integrated so the heart rate can change smoothly without jumps. Always simulated.
    static float hr = HR_CALM, phase = 0, last_t = 0;
    float dt = t - last_t;
    last_t = t;
    if (dt <= 0 || dt > 1) {
        dt = 1.0f / 64;
    }
    approach(&hr, s_stressed ? HR_STRESS : HR_CALM, dt);
    phase = fmodf(phase + dt * hr / 60.0f, 1.0f);
    float sys = expf(-powf((phase - 0.15f) / 0.06f, 2));
    float dic = 0.35f * expf(-powf((phase - 0.45f) / 0.08f, 2));
    return 80.0f * (sys + dic) - 30.0f + noise(3.0f);
}

void signals_acc(float t, int16_t out[3])
{
    // Wrist mostly still, gravity on z. Always simulated (no IMU wired yet).
    float jitter = s_stressed ? 4.0f : 2.0f;
    (void)t;
    out[0] = (int16_t)lroundf(noise(jitter));
    out[1] = (int16_t)lroundf(noise(jitter));
    out[2] = (int16_t)lroundf(64.0f + noise(jitter));
}

bool signals_moisture(float *patch, float *reference)
{
#if defined(CONFIG_DERMEDGE_MOISTURE_TOUCH)
    uint16_t p = 0, r = 0;
    touch_pad_read(TOUCH_PAD_NUM0, &p);
    touch_pad_read(TOUCH_PAD_NUM3, &r);
    if (p == 0 || r == 0) {
        return false;
    }
    // More capacitance (wetter skin) lowers the touch count, so invert to "higher = wetter".
    *patch = 100000.0f / p;
    *reference = 100000.0f / r;
    return true;
#elif defined(CONFIG_DERMEDGE_MOISTURE_SIMULATED)
    // Southampton patients read 1.7-10.2% lower on eczema skin; simulate a 6% deficit.
    *reference = 50.0f + noise(0.3f);
    *patch = 47.0f + noise(0.3f);
    return true;
#else
    (void)patch;
    (void)reference;
    return false;
#endif
}
