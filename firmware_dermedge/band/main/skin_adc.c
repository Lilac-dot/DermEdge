#include "skin_adc.h"

#include <math.h>
#include <stdio.h>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "nvs.h"
#include "skin.h"

#define GSR_READS 8   // averaged per 4 Hz sample
#define TEMP_READS 32 // one ESP32 ADC read is noisy by a few mV, several tenths of a degree
#define NVS_KEY "gsr_noskin"
#define FLOAT_CHECK_EVERY 8    // 4 Hz samples between "is anything driving the GSR pin?" checks: 2 s
#define FLOAT_SWING_MV 300     // a floating pin follows the internal pulls by far more than this
#define TEMP_STEADY_C 1.0f     // real skin temperature never moves this much in a second
#define TEMP_STEADY_N 3        // seconds of steady readings before the thermistor counts

static const char *TAG = "skin";
static adc_oneshot_unit_handle_t adc;
static adc_cali_handle_t cali;
static adc_channel_t gsr_ch, temp_ch;
static volatile bool sim, sim_stressed, debug;
static float no_skin = SKIN_GSR_NO_SKIN;

static float read_mv(adc_channel_t ch, int reads) {
    int sum = 0, n = 0;
    for (int i = 0; i < reads; i++) {
        int raw;
        if (adc_oneshot_read(adc, ch, &raw) == ESP_OK) sum += raw, n++;
    }
    int mv = 0;
    if (n) adc_cali_raw_to_voltage(cali, sum / n, &mv);
    return mv;
}

static void load_calibration(void) {
    nvs_handle_t h;
    uint32_t v;
    if (nvs_open("band", NVS_READONLY, &h) != ESP_OK) return;
    if (nvs_get_u32(h, NVS_KEY, &v) == ESP_OK) no_skin = v / 100.0f;
    nvs_close(h);
}

bool skin_gsr_calibrate(float *out) {
    float a = 0;
    for (int i = 0; i < 16; i++) {
        a += skin_gsr_reading(read_mv(gsr_ch, GSR_READS), SKIN_SUPPLY_MV) / 16;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    *out = a;
    if (a < 300 || a > 700) return false;  // the module reads about 512 with nothing on it
    nvs_handle_t h;
    if (nvs_open("band", NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_set_u32(h, NVS_KEY, (uint32_t)(a * 100));
    nvs_commit(h);
    nvs_close(h);
    no_skin = a;
    return true;
}

// An unplugged GSR pin floats, and a floating pin can sit at any voltage, even one that reads as
// skin. A module's output holds its voltage against the ESP32's weak internal pulls; a floating
// pin follows them. GPIO 32 has internal pulls (GPIO 34, the thermistor's, doesn't).
static bool gsr_pin_floating(void) {
    gpio_set_pull_mode(SKIN_GSR_GPIO, GPIO_PULLDOWN_ONLY);
    vTaskDelay(pdMS_TO_TICKS(2));
    float low = read_mv(gsr_ch, 4);
    gpio_set_pull_mode(SKIN_GSR_GPIO, GPIO_PULLUP_ONLY);
    vTaskDelay(pdMS_TO_TICKS(2));
    float high = read_mv(gsr_ch, 4);
    gpio_set_pull_mode(SKIN_GSR_GPIO, GPIO_FLOATING);
    return high - low > FLOAT_SWING_MV;
}

void skin_set_sim(bool on) { sim = on; }
void skin_set_sim_stressed(bool on) { sim_stressed = on; }
void skin_set_debug(bool on) { debug = on; }

static void skin_task(void *arg) {
    QueueHandle_t out = arg;
    skin_sim_t s;
    skin_sim_init(&s, 0x5EED);
    skin_reading_t r = {0};
    bool floating = true;
    float last_c = 0;
    int steady = 0;
    TickType_t wake = xTaskGetTickCount();
    for (int n = 0;; n++) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(1000 / SKIN_GSR_HZ));
        r.gsr_mv = read_mv(gsr_ch, GSR_READS);
        if (n % FLOAT_CHECK_EVERY == 0) floating = gsr_pin_floating();
        bool new_temp = n % SKIN_GSR_HZ == 0;
        if (new_temp) r.temp_mv = read_mv(temp_ch, TEMP_READS);  // temperature once a second
        r.sim = sim;
        if (sim) {
            s.stressed = sim_stressed;
            skin_sim_step(&s, &r.us, &r.c);
        } else {
            r.us = skin_gsr_us(r.gsr_mv, SKIN_SUPPLY_MV, no_skin);
            r.c = skin_ntc_c(r.temp_mv, SKIN_SUPPLY_MV, SKIN_NTC_SERIES_R, SKIN_NTC_R25, SKIN_NTC_BETA);
        }
        r.gsr_ok = skin_gsr_plausible(r.us) && (sim || !floating);
        if (new_temp) {  // count it only after a few steady seconds in range: a floating pin wanders
            bool in_range = skin_temp_plausible(r.c);
            steady = in_range && fabsf(r.c - last_c) < TEMP_STEADY_C ? steady + 1 : 0;
            last_c = r.c;
        }
        r.temp_ok = skin_temp_plausible(r.c) && (sim || steady >= TEMP_STEADY_N);
        r.gsr_connected = sim || !floating;
        r.temp_settling = skin_temp_plausible(r.c) && !r.temp_ok;
        xQueueSend(out, &r, 0);
        if (debug && n % SKIN_GSR_HZ == 0)
            printf("skin: GSR pin %4.0f mV (reading %3.0f, no-skin %3.0f) -> %s | thermistor pin %4.0f mV -> %s%s\n",
                   r.gsr_mv, skin_gsr_reading(r.gsr_mv, SKIN_SUPPLY_MV), no_skin,
                   r.gsr_ok ? "on skin" : floating && !sim ? "NOTHING CONNECTED (pin floating)" :
                   r.us > SKIN_GSR_MAX_US ? "electrodes touching" : "not on skin",
                   r.temp_mv, r.temp_ok ? "ok" : skin_temp_plausible(r.c) ? "settling (or floating)" : "unplugged or shorted",
                   sim ? "  [SIMULATED]" : "");
        if (debug && n % SKIN_GSR_HZ == 0 && (r.gsr_ok || r.temp_ok))
            printf("skin:   %.2f uS, %.1f C\n", r.us, r.c);
    }
}

void skin_adc_start(QueueHandle_t out) {
    adc_oneshot_unit_init_cfg_t unit = {.unit_id = ADC_UNIT_1};
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit, &adc));
    adc_unit_t u;
    ESP_ERROR_CHECK(adc_oneshot_io_to_channel(SKIN_GSR_GPIO, &u, &gsr_ch));
    ESP_ERROR_CHECK(adc_oneshot_io_to_channel(SKIN_TEMP_GPIO, &u, &temp_ch));
    adc_oneshot_chan_cfg_t ch = {.bitwidth = ADC_BITWIDTH_12, .atten = ADC_ATTEN_DB_12};  // 0-3.1 V
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc, gsr_ch, &ch));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc, temp_ch, &ch));
    adc_cali_line_fitting_config_t c = {.unit_id = ADC_UNIT_1, .atten = ADC_ATTEN_DB_12,
                                        .bitwidth = ADC_BITWIDTH_12, .default_vref = 1100};
    ESP_ERROR_CHECK(adc_cali_create_scheme_line_fitting(&c, &cali));
    load_calibration();
    ESP_LOGI(TAG, "GSR on GPIO %d (no-skin reading %.0f), thermistor on GPIO %d", SKIN_GSR_GPIO, no_skin,
             SKIN_TEMP_GPIO);
    xTaskCreatePinnedToCore(skin_task, "skin", 3072, out, 4, NULL, 1);
}
