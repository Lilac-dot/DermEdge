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
#include "nvs.h"
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
static volatile float temp_offset;        // from "temp cal", added to every reading
static volatile float raw_c_last = -1000; // the last second's uncalibrated reading
static volatile uint32_t raw_c_seq;       // counts seconds, for the calibration to wait on
#define NVS_TEMP_KEY "temp_off"           // hundredths of a degree, signed

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
        float raw_c = skin_lm35_c(r.temp_mv);
        raw_c_last = raw_c, raw_c_seq++;
        r.c = sim ? skin_sim_temp(&s) : raw_c + temp_offset;
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
            printf("skin: ECG pin %4.0f mV, electrodes %s, %d beat%s, heart rate %s%.0f | LM35 pin %4.0f mV -> %.1f C (offset %+.1f) %s%s\n",
                   r.ecg_mv, r.ecg_on ? "on" : "OFF", r.n_beats, r.n_beats == 1 ? "" : "s",
                   r.hr_bpm > 0 ? "" : "-- ", r.hr_bpm, r.temp_mv, r.c, sim ? 0.0f : temp_offset,
                   r.temp_ok ? "ok" : in_range ? "settling" : "unplugged or out of range", sim ? "  [SIMULATED]" : "");
        i = 0, on_samples = 0, temp_n = 0, temp_sum = 0;
        r.n_beats = 0;
    }
}

static void save_offset(float off) {
    nvs_handle_t h;
    if (nvs_open("band", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, NVS_TEMP_KEY, (int32_t)lroundf(off * 100));
    nvs_commit(h);
    nvs_close(h);
}

static void load_offset(void) {
    nvs_handle_t h;
    int32_t v;
    if (nvs_open("band", NVS_READONLY, &h) != ESP_OK) return;
    if (nvs_get_i32(h, NVS_TEMP_KEY, &v) == ESP_OK) temp_offset = v / 100.0f;
    nvs_close(h);
}

bool skin_temp_calibrate(float reference_c, float *offset, float *measured) {
    float sum = 0, lo = 1000, hi = -1000;
    int n = 0;
    uint32_t seen = raw_c_seq;
    for (int waited = 0; n < 10 && waited < 150; waited++) {  // 10 fresh seconds, giving up after 15 s
        vTaskDelay(pdMS_TO_TICKS(100));
        if (raw_c_seq == seen) continue;
        seen = raw_c_seq;
        float c = raw_c_last;
        sum += c, n++;
        lo = fminf(lo, c), hi = fmaxf(hi, c);
    }
    *measured = n ? sum / n : -1000;
    if (n < 10 || hi - lo > 1.0f) return false;  // not steady: a loose wire, or still settling
    if (!skin_temp_offset(*measured, reference_c, offset)) return false;
    temp_offset = *offset;
    save_offset(*offset);
    return true;
}

void skin_temp_cal_clear(void) {
    temp_offset = 0;
    save_offset(0);
}

float skin_temp_cal_offset(void) { return temp_offset; }

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
    load_offset();
    ESP_LOGI(TAG, "ECG on GPIO %d (lead-off %d/%d), LM35 on GPIO %d (calibration offset %+.2f C), %d Hz",
             SKIN_ECG_GPIO, SKIN_LO_PLUS_GPIO, SKIN_LO_MINUS_GPIO, SKIN_TEMP_GPIO, temp_offset, SKIN_ECG_HZ);
    xTaskCreatePinnedToCore(skin_task, "skin", 4096, out, 6, &task, 1);
    esp_timer_handle_t timer;
    esp_timer_create_args_t args = {.callback = tick, .name = "ecg"};
    ESP_ERROR_CHECK(esp_timer_create(&args, &timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, 1000000 / SKIN_ECG_HZ));
}
