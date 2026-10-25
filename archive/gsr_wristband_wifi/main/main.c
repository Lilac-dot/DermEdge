// DermEdge wristband on a plain ESP32.
//
// Samples at the Empatica E4 rates the WESAD stress model was trained on -- skin conductance
// (GSR/EDA) and skin temperature at 4 Hz, PPG (BVP) at 64 Hz, accelerometer at 32 Hz -- and
// every CONFIG_DERMEDGE_WINDOW_SECONDS POSTs the raw window to the laptop hub's
// /api/wearable/window, where the stress model runs (the hub calibrates it on the first 3
// windows, so start calm). The moisture patch goes to /api/wearable/moisture.
//
// With simulated GSR, the BOOT button toggles calm <-> stressed for a live demo.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "signals.h"
#include "dermedge_net.h"

static const char *TAG = "wristband";

#define SAMPLE_HZ 64
#define WIN_S CONFIG_DERMEDGE_WINDOW_SECONDS
#define N_BVP (WIN_S * 64)
#define N_ACC (WIN_S * 32)
#define N_EDA (WIN_S * 4)
#define N_MOIST (WIN_S)  // one moisture reading per second
#define JSON_CAP (48 * 1024)

typedef struct {
    float eda[N_EDA], temp[N_EDA], bvp[N_BVP];
    int16_t acc[N_ACC][3];
    float patch[N_MOIST], ref[N_MOIST];
    int n_moist;
} window_t;

static window_t s_win[2];        // sampler fills one while the sender posts the other
static int s_fill = 0;           // index being filled
static uint32_t s_tick = 0;      // 64 Hz sample counter within the window
static uint32_t s_total_ticks = 0;
static QueueHandle_t s_ready;    // indices of finished windows

static void sample_tick(void *arg)
{
    (void)arg;
    float t = s_total_ticks / (float)SAMPLE_HZ;
    window_t *w = &s_win[s_fill];

    // BOOT button: falling edge toggles the simulated calm/stressed state (debounced).
    static int last_level = 1;
    static uint32_t last_press = 0;
    int level = gpio_get_level(CONFIG_DERMEDGE_STRESS_BUTTON_GPIO);
    if (level == 0 && last_level == 1 && s_total_ticks - last_press > SAMPLE_HZ / 4) {
        signals_set_stressed(!signals_stressed());
        last_press = s_total_ticks;
        ESP_LOGI(TAG, "simulated state: %s", signals_stressed() ? "STRESSED" : "calm");
    }
    last_level = level;

    w->bvp[s_tick] = signals_bvp(t);
    if (s_tick % 2 == 0) {
        signals_acc(t, w->acc[s_tick / 2]);
    }
    if (s_tick % 16 == 0) {
        w->eda[s_tick / 16] = signals_eda(t);
        w->temp[s_tick / 16] = signals_temp(t);
    }
    if (s_tick % SAMPLE_HZ == 0 && w->n_moist < N_MOIST &&
        signals_moisture(&w->patch[w->n_moist], &w->ref[w->n_moist])) {
        w->n_moist++;
    }

    s_total_ticks++;
    if (++s_tick == N_BVP) {
        int done = s_fill;
        if (xQueueSend(s_ready, &done, 0) == pdTRUE) {
            s_fill ^= 1;  // the sender owns `done` until it posts it
        } else {
            ESP_LOGW(TAG, "previous window still sending -- overwriting this one");
        }
        s_tick = 0;
        s_win[s_fill].n_moist = 0;
    }
}

static int append(char *buf, int pos, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static int append(char *buf, int pos, const char *fmt, ...)
{
    if (pos < 0 || pos >= JSON_CAP) {
        return -1;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + pos, JSON_CAP - pos, fmt, ap);
    va_end(ap);
    return (n < 0 || pos + n >= JSON_CAP) ? -1 : pos + n;
}

static int append_floats(char *buf, int pos, const char *key, const float *v, int n, const char *fmt)
{
    pos = append(buf, pos, "\"%s\":[", key);
    for (int i = 0; i < n && pos >= 0; i++) {
        pos = append(buf, pos, fmt, v[i]);
        if (i < n - 1) {
            pos = append(buf, pos, ",");
        }
    }
    return append(buf, pos, "]");
}

static void sender_task(void *arg)
{
    (void)arg;
    char *json = malloc(JSON_CAP);
    char resp[512];
    int windows_sent = 0;
    if (!json) {
        ESP_LOGE(TAG, "no memory for the JSON buffer");
        vTaskDelete(NULL);
    }
    while (true) {
        int idx;
        xQueueReceive(s_ready, &idx, portMAX_DELAY);
        const window_t *w = &s_win[idx];

        int pos = append(json, 0, "{");
        pos = append_floats(json, pos, "eda", w->eda, N_EDA, "%.3f");
        pos = append(json, pos, ",");
        pos = append_floats(json, pos, "temp", w->temp, N_EDA, "%.2f");
        pos = append(json, pos, ",");
        pos = append_floats(json, pos, "bvp", w->bvp, N_BVP, "%.1f");
        pos = append(json, pos, ",\"acc\":[");
        for (int i = 0; i < N_ACC && pos >= 0; i++) {
            pos = append(json, pos, "[%d,%d,%d]%s", w->acc[i][0], w->acc[i][1], w->acc[i][2],
                         i < N_ACC - 1 ? "," : "");
        }
        pos = append(json, pos, "]}");
        if (pos < 0) {
            ESP_LOGE(TAG, "window JSON overflowed %d bytes", JSON_CAP);
            continue;
        }
        int status = dermedge_hub_post_json("/api/wearable/window", json, pos, resp, sizeof(resp));
        windows_sent++;
        ESP_LOGI(TAG, "window %d (%s) -> HTTP %d%s", windows_sent,
                 signals_stressed() ? "stressed" : "calm", status,
                 windows_sent <= 3 ? "  [hub is calibrating to this wearer -- stay calm]" : "");

        if (w->n_moist > 0) {
            pos = append(json, 0, "{");
            pos = append_floats(json, pos, "patch", w->patch, w->n_moist, "%.2f");
            pos = append(json, pos, ",");
            pos = append_floats(json, pos, "reference", w->ref, w->n_moist, "%.2f");
            pos = append(json, pos, "}");
            if (pos > 0) {
                status = dermedge_hub_post_json("/api/wearable/moisture", json, pos, resp, sizeof(resp));
                ESP_LOGI(TAG, "moisture (%d readings) -> HTTP %d", w->n_moist, status);
            }
        }
    }
}

void app_main(void)
{
    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << CONFIG_DERMEDGE_STRESS_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn);
    signals_init();
    if (signals_gsr_simulated()) {
        ESP_LOGW(TAG, "GSR is SIMULATED -- press BOOT to toggle calm/stressed");
    }
    ESP_LOGW(TAG, "%s are simulated until those sensors are wired",
             signals_temp_simulated() ? "skin temperature, PPG and accelerometer" : "PPG and accelerometer");

    while (dermedge_wifi_connect(20000) != ESP_OK) {
        ESP_LOGW(TAG, "can't join \"%s\" yet, retrying", CONFIG_DERMEDGE_WIFI_SSID);
    }

    s_ready = xQueueCreate(1, sizeof(int));
    xTaskCreate(sender_task, "sender", 8192, NULL, 5, NULL);

    const esp_timer_create_args_t args = {
        .callback = sample_tick,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "sampler",
    };
    esp_timer_handle_t timer;
    ESP_ERROR_CHECK(esp_timer_create(&args, &timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, 1000000 / SAMPLE_HZ));
    ESP_LOGI(TAG, "sampling; first window in %d s", WIN_S);
}
