#include "skin_adc.h"

#include <math.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "skin.h"

#define TEMP_STEADY_C 1.0f     // real skin temperature never moves this much in a second
#define TEMP_STEADY_N 3        // seconds of steady readings before the LM35 counts
#define ECG_ON_SAMPLES (SKIN_ECG_HZ * 9 / 10)  // electrodes on for 90% of a second counts as on

static const char *TAG = "skin";
static adc_oneshot_unit_handle_t adc;
static adc_cali_handle_t cali_ecg, cali_temp;
static adc_channel_t ecg_ch, temp_ch;
static volatile bool sim, sim_stressed, debug;
static TaskHandle_t task;

static int read_mv(adc_channel_t ch, adc_cali_handle_t cali) {
    int raw = 0, mv = 0;
    if (adc_oneshot_read(adc, ch, &raw) != ESP_OK) return -1;
    adc_cali_raw_to_voltage(cali, raw, &mv);
    return mv;
}

void skin_set_sim(bool on) { sim = on; }
void skin_set_sim_stressed(bool on) { sim_stressed = on; }
void skin_set_debug(bool on) { debug = on; }

// 250 Hz from a hardware timer: the FreeRTOS tick (100 Hz) is too coarse
static void tick(void *arg) { xTaskNotifyGive(task); }

static void skin_task(void *arg) {
    QueueHandle_t out = arg;
    skin_sim_t s;
    skin_sim_init(&s, 0x5EED);
    ecg_det_t det;
    ecg_det_init(&det);
    skin_reading_t r = {0};
    float last_c = 0;
    int steady = 0, off_run = 0, on_samples = 0, temp_n = 0, i = 0;
    float temp_sum = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        // ECG: either lead-off pin high means an electrode is off; two samples in a row, so a
        // single glitch doesn't reset the beat detector
        float mv;
        bool on;
        if (sim) {
            s.stressed = sim_stressed;
            mv = skin_sim_ecg(&s);
            on = true;
        } else {
            mv = read_mv(ecg_ch, cali_ecg);
            off_run = gpio_get_level(SKIN_LO_PLUS_GPIO) || gpio_get_level(SKIN_LO_MINUS_GPIO) ? off_run + 1 : 0;
            on = off_run < 2;
        }
        r.ecg_mv = mv;
        on_samples += on;
        int rr;
        bool valid;
        if (ecg_det_feed(&det, mv, on, &rr, &valid) && r.n_beats < SKIN_MAX_BEATS) {
            r.rr_ms[r.n_beats] = rr;
            r.rr_valid[r.n_beats++] = valid;
        }
        int t_mv = read_mv(temp_ch, cali_temp);
        if (t_mv >= 0) temp_sum += t_mv, temp_n++;

        if (++i < SKIN_ECG_HZ) continue;
        // once a second: post it
        r.temp_mv = temp_n ? temp_sum / temp_n : -1;
        r.c = sim ? skin_sim_temp(&s) : skin_lm35_c(r.temp_mv);
        // count it only after a few steady seconds in range: a floating pin wanders
        bool in_range = skin_temp_plausible(r.c);
        steady = in_range && fabsf(r.c - last_c) < TEMP_STEADY_C ? steady + 1 : 0;
        last_c = r.c;
        r.temp_ok = in_range && (sim || steady >= TEMP_STEADY_N);
        r.temp_settling = in_range && !r.temp_ok;
        r.ecg_on = on_samples >= ECG_ON_SAMPLES;
        r.hr_bpm = r.ecg_on ? ecg_det_bpm(&det) : 0;
        r.sim = sim;
        xQueueSend(out, &r, 0);
        if (debug)
            printf("skin: ECG pin %4.0f mV, electrodes %s, %d beat%s, heart rate %s%.0f | LM35 pin %4.0f mV -> %.1f C %s%s\n",
                   r.ecg_mv, r.ecg_on ? "on" : "OFF", r.n_beats, r.n_beats == 1 ? "" : "s",
                   r.hr_bpm > 0 ? "" : "-- ", r.hr_bpm, r.temp_mv, r.c,
                   r.temp_ok ? "ok" : in_range ? "settling" : "unplugged or out of range", sim ? "  [SIMULATED]" : "");
        i = 0, on_samples = 0, temp_n = 0, temp_sum = 0;
        r.n_beats = 0;
    }
}

static adc_cali_handle_t make_cali(adc_atten_t atten) {
    adc_cali_line_fitting_config_t c = {.unit_id = ADC_UNIT_1, .atten = atten, .bitwidth = ADC_BITWIDTH_12,
                                        .default_vref = 1100};
    adc_cali_handle_t h;
    ESP_ERROR_CHECK(adc_cali_create_scheme_line_fitting(&c, &h));
    return h;
}

void skin_adc_start(QueueHandle_t out) {
    adc_oneshot_unit_init_cfg_t unit = {.unit_id = ADC_UNIT_1};
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit, &adc));
    adc_unit_t u;
    ESP_ERROR_CHECK(adc_oneshot_io_to_channel(SKIN_ECG_GPIO, &u, &ecg_ch));
    ESP_ERROR_CHECK(adc_oneshot_io_to_channel(SKIN_TEMP_GPIO, &u, &temp_ch));
    // the ECG swings across most of 0-3.3 V; the LM35 gives 0.30-0.36 V on skin, so 0 dB
    // (about 0-950 mV) gives it the finest steps
    adc_oneshot_chan_cfg_t ecg = {.bitwidth = ADC_BITWIDTH_12, .atten = ADC_ATTEN_DB_12};
    adc_oneshot_chan_cfg_t temp = {.bitwidth = ADC_BITWIDTH_12, .atten = ADC_ATTEN_DB_0};
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc, ecg_ch, &ecg));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc, temp_ch, &temp));
    cali_ecg = make_cali(ADC_ATTEN_DB_12);
    cali_temp = make_cali(ADC_ATTEN_DB_0);
    // the AD8232 drives LO+/LO- push-pull; 34 is input-only with no pulls anyway
    gpio_config_t lo = {.pin_bit_mask = (1ULL << SKIN_LO_PLUS_GPIO) | (1ULL << SKIN_LO_MINUS_GPIO),
                        .mode = GPIO_MODE_INPUT};
    ESP_ERROR_CHECK(gpio_config(&lo));
    ESP_LOGI(TAG, "ECG on GPIO %d (lead-off %d/%d), LM35 on GPIO %d, %d Hz", SKIN_ECG_GPIO, SKIN_LO_PLUS_GPIO,
             SKIN_LO_MINUS_GPIO, SKIN_TEMP_GPIO, SKIN_ECG_HZ);
    xTaskCreatePinnedToCore(skin_task, "skin", 4096, out, 6, &task, 1);
    esp_timer_handle_t timer;
    esp_timer_create_args_t args = {.callback = tick, .name = "ecg"};
    ESP_ERROR_CHECK(esp_timer_create(&args, &timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, 1000000 / SKIN_ECG_HZ));
}
