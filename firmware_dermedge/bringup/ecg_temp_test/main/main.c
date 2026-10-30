// DermEdge ECG + LM35 bring-up on a plain ESP32.
//
// Samples the AD8232's OUTPUT (GPIO 32) at 250 Hz and its lead-off pins LO+ (33) and LO- (34),
// plus the LM35 (GPIO 35) once per ECG sample, averaged over each second, and the touch pad's
// OUT1 (GPIO 27, high while touched). Streams one line per
// ECG sample for tools/plot.py, and once a second a summary line starting with '#':
//   E <ecg_mv> <leads_on>          250 per second
//   # temp 33.1 C  hr 72 bpm  leads on
//   # touch pressed / # touch released   on every change
// The heart rate is a rough R-peak count to show the wiring works, not the band's detector.
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ecg_temp_test";

#define ECG_CHANNEL ADC_CHANNEL_4   // GPIO 32
#define LM35_CHANNEL ADC_CHANNEL_7  // GPIO 35
#define PIN_LO_PLUS 33
#define PIN_LO_MINUS 34
#define PIN_TOUCH 27

#define FS_HZ 250
#define REFRACTORY_MS 250   // no two beats closer than this (240 bpm)
#define MIN_PEAK_MV 40      // ignore wiggles smaller than this

static adc_oneshot_unit_handle_t adc;
static adc_cali_handle_t cali_ecg, cali_lm35;

// Line-fitting calibration from the chip's eFuse values; NULL if this chip has none.
static adc_cali_handle_t make_cali(adc_atten_t atten) {
    adc_cali_line_fitting_config_t cfg = {.unit_id = ADC_UNIT_1, .atten = atten,
                                          .bitwidth = ADC_BITWIDTH_12};
    adc_cali_handle_t h = NULL;
    if (adc_cali_create_scheme_line_fitting(&cfg, &h) != ESP_OK) {
        ESP_LOGW(TAG, "no ADC calibration in eFuse for atten %d, using the nominal range", atten);
        return NULL;
    }
    return h;
}

static int read_mv(adc_channel_t ch, adc_cali_handle_t cali, int full_scale_mv) {
    int raw = 0, mv = 0;
    ESP_ERROR_CHECK(adc_oneshot_read(adc, ch, &raw));
    if (cali && adc_cali_raw_to_voltage(cali, raw, &mv) == ESP_OK) return mv;
    return raw * full_scale_mv / 4095;
}

static void setup(void) {
    adc_oneshot_unit_init_cfg_t unit = {.unit_id = ADC_UNIT_1};
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit, &adc));
    // The ECG swings across most of 0-3.3 V; the LM35 gives 0.30-0.36 V on skin, so 0 dB
    // (about 0-950 mV) gives it the finest steps.
    adc_oneshot_chan_cfg_t ecg = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12};
    adc_oneshot_chan_cfg_t lm35 = {.atten = ADC_ATTEN_DB_0, .bitwidth = ADC_BITWIDTH_12};
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc, ECG_CHANNEL, &ecg));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc, LM35_CHANNEL, &lm35));
    cali_ecg = make_cali(ADC_ATTEN_DB_12);
    cali_lm35 = make_cali(ADC_ATTEN_DB_0);

    // The AD8232 drives LO+/LO- push-pull, and 34 has no pull resistors anyway.
    gpio_config_t lo = {.pin_bit_mask = (1ULL << PIN_LO_PLUS) | (1ULL << PIN_LO_MINUS),
                        .mode = GPIO_MODE_INPUT};
    ESP_ERROR_CHECK(gpio_config(&lo));
    gpio_config_t touch = {.pin_bit_mask = 1ULL << PIN_TOUCH, .mode = GPIO_MODE_INPUT,
                           .pull_down_en = GPIO_PULLDOWN_ENABLE};  // reads low if OUT1 comes off
    ESP_ERROR_CHECK(gpio_config(&touch));
}

void app_main(void) {
    setup();
    ESP_LOGI(TAG, "ECG on GPIO 32 (LO+ 33, LO- 34), LM35 on GPIO 35, touch on 27, %d Hz", FS_HZ);

    float baseline = -1, envelope = 0;
    int64_t t_ms = 0, last_beat_ms = -REFRACTORY_MS;
    bool above = false, touched = false;
    int rr_ms[8] = {0}, n_rr = 0;
    int lm35_sum = 0, lm35_n = 0, off_samples = 0;
    TickType_t wake = xTaskGetTickCount();

    for (int i = 0;; i++, t_ms += 1000 / FS_HZ) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(1000 / FS_HZ));

        // Either lead-off pin going high means an electrode is off. Needing it for 2 samples
        // in a row skips single-sample glitches.
        bool off_now = gpio_get_level(PIN_LO_PLUS) || gpio_get_level(PIN_LO_MINUS);
        off_samples = off_now ? off_samples + 1 : 0;
        bool leads_on = off_samples < 2;

        bool touch_now = gpio_get_level(PIN_TOUCH);
        if (touch_now != touched) printf("# touch %s\n", touch_now ? "pressed" : "released");
        touched = touch_now;

        int ecg = read_mv(ECG_CHANNEL, cali_ecg, 3100);
        lm35_sum += read_mv(LM35_CHANNEL, cali_lm35, 950);
        lm35_n++;
        printf("E %d %d\n", ecg, leads_on);

        // Rough R-peaks: distance from a slow baseline, against a decaying envelope of recent
        // peaks. Works with either electrode polarity.
        if (baseline < 0) baseline = ecg;
        baseline += (ecg - baseline) / 64.0f;  // about 0.25 s
        float dev = fabsf(ecg - baseline);
        envelope = fmaxf(envelope * 0.995f, dev);
        bool now_above = leads_on && dev > MIN_PEAK_MV && dev > 0.6f * envelope;
        if (now_above && !above && t_ms - last_beat_ms >= REFRACTORY_MS) {
            int rr = (int)(t_ms - last_beat_ms);
            if (rr < 2000) rr_ms[n_rr++ % 8] = rr;
            last_beat_ms = t_ms;
        }
        above = now_above;

        if ((i + 1) % FS_HZ == 0) {
            float temp_c = (float)lm35_sum / lm35_n / 10.0f;  // 10 mV per degree
            int n = n_rr < 8 ? n_rr : 8, sum = 0;
            for (int k = 0; k < n; k++) sum += rr_ms[k];
            bool fresh = t_ms - last_beat_ms < 3000;
            if (n >= 3 && fresh)
                printf("# temp %.1f C  hr %d bpm  leads %s\n", temp_c, 60000 * n / sum,
                       leads_on ? "on" : "OFF");
            else
                printf("# temp %.1f C  hr --  leads %s\n", temp_c, leads_on ? "on" : "OFF");
            if (!fresh) n_rr = 0;
            lm35_sum = lm35_n = 0;
        }
    }
}
