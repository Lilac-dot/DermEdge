// SkinCheck tier 1 on an AI-Thinker ESP32-CAM.
//
// Every CONFIG_SKINCHECK_CAPTURE_PERIOD_MS: capture a JPEG, decide normal vs eczema/rash
// (on device with the INT8 model, or on the laptop hub), light the red LED while positive,
// and after CONFIG_SKINCHECK_POSITIVE_STREAK positives in a row send the photo to the hub's
// /api/analyze -- eczema vs look-alikes, severity, flare risk, history -- which then shows
// on the dashboard. Nothing leaves the local network.
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "driver/gpio.h"
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "img_converters.h"
#include "screen_model.h"
#include "skincheck_net.h"

static const char *TAG = "skincheck_cam";

// AI-Thinker ESP32-CAM pin map
#define CAM_PIN_PWDN 32
#define CAM_PIN_RESET -1
#define CAM_PIN_XCLK 0
#define CAM_PIN_SIOD 26
#define CAM_PIN_SIOC 27
#define CAM_PIN_D7 35
#define CAM_PIN_D6 34
#define CAM_PIN_D5 39
#define CAM_PIN_D4 36
#define CAM_PIN_D3 21
#define CAM_PIN_D2 19
#define CAM_PIN_D1 18
#define CAM_PIN_D0 5
#define CAM_PIN_VSYNC 25
#define CAM_PIN_HREF 23
#define CAM_PIN_PCLK 22
#define LED_RED_GPIO 33    // small red LED on the back, active low
#define LED_FLASH_GPIO 4   // bright white flash LED, active high

#define FRAME_W 320
#define FRAME_H 240

// Kconfig leaves a bool option undefined when it is off, so turn each into a 0/1 constant.
#ifdef CONFIG_SKINCHECK_SCREEN_ON_DEVICE
#define SCREEN_ON_DEVICE 1
#else
#define SCREEN_ON_DEVICE 0
#endif
#ifdef CONFIG_SKINCHECK_SWAP_RB
#define SWAP_RB true
#else
#define SWAP_RB false
#endif
#ifdef CONFIG_SKINCHECK_FLASH_DURING_CAPTURE
#define FLASH_DURING_CAPTURE 1
#else
#define FLASH_DURING_CAPTURE 0
#endif

static void led_red(bool on) { gpio_set_level(LED_RED_GPIO, on ? 0 : 1); }

static void leds_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << LED_RED_GPIO) | (1ULL << LED_FLASH_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    led_red(false);
    gpio_set_level(LED_FLASH_GPIO, 0);
}

static esp_err_t camera_init(void)
{
    camera_config_t cfg = {
        .pin_pwdn = CAM_PIN_PWDN,
        .pin_reset = CAM_PIN_RESET,
        .pin_xclk = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d7 = CAM_PIN_D7,
        .pin_d6 = CAM_PIN_D6,
        .pin_d5 = CAM_PIN_D5,
        .pin_d4 = CAM_PIN_D4,
        .pin_d3 = CAM_PIN_D3,
        .pin_d2 = CAM_PIN_D2,
        .pin_d1 = CAM_PIN_D1,
        .pin_d0 = CAM_PIN_D0,
        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href = CAM_PIN_HREF,
        .pin_pclk = CAM_PIN_PCLK,
        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,  // JPEG for the hub; decoded to RGB for the model
        .frame_size = FRAMESIZE_QVGA,    // 320x240
        .jpeg_quality = 12,
        .fb_count = 2,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };
    return esp_camera_init(&cfg);
}

// Returns P(positive) for this frame, or -1 if it couldn't be scored.
static float screen_frame(camera_fb_t *fb, uint8_t *rgb)
{
#if SCREEN_ON_DEVICE
    if (!fmt2rgb888(fb->buf, fb->len, fb->format, rgb)) {
        ESP_LOGW(TAG, "JPEG decode failed");
        return -1.f;
    }
    int64_t t0 = esp_timer_get_time();
    float p = screen_model_predict(rgb, fb->width, fb->height, SWAP_RB);
    ESP_LOGI(TAG, "on-device P(eczema) %.2f in %lld ms", p, (esp_timer_get_time() - t0) / 1000);
    return p;
#else
    static char resp[256];
    int status = skincheck_hub_post_jpeg("/api/screen", fb->buf, fb->len, resp, sizeof(resp));
    if (status != 200) {
        ESP_LOGW(TAG, "hub screen failed (HTTP %d)", status);
        return -1.f;
    }
    cJSON *j = cJSON_Parse(resp);
    float p = -1.f;
    if (j) {
        cJSON *pr = cJSON_GetObjectItem(j, "p_rash"), *skin = cJSON_GetObjectItem(j, "skin");
        if (cJSON_IsNumber(pr)) {
            p = cJSON_IsFalse(skin) ? 0.f : (float)pr->valuedouble;  // no skin in view -> not a rash
        }
        cJSON_Delete(j);
    }
    ESP_LOGI(TAG, "hub P(rash) %.2f", p);
    return p;
#endif
}

static void send_for_analysis(camera_fb_t *fb)
{
    static char resp[2048];
    ESP_LOGI(TAG, "sending %u-byte photo for full analysis", (unsigned)fb->len);
    int status = skincheck_hub_post_jpeg("/api/analyze?source=esp32", fb->buf, fb->len, resp, sizeof(resp));
    if (status != 200) {
        ESP_LOGW(TAG, "hub analyze failed (HTTP %d)", status);
        return;
    }
    cJSON *j = cJSON_Parse(resp);
    if (j) {
        cJSON *h = cJSON_GetObjectItem(j, "headline");
        ESP_LOGI(TAG, "hub says: %s", cJSON_IsString(h) ? h->valuestring : "(no headline)");
        cJSON_Delete(j);
    }
}

void app_main(void)
{
    leds_init();
    led_red(true);  // on while starting up

    if (camera_init() != ESP_OK) {
        ESP_LOGE(TAG, "camera init failed -- check the ribbon cable and that PSRAM is enabled");
        return;
    }
    uint8_t *rgb = NULL;
#if SCREEN_ON_DEVICE
    if (!screen_model_init()) {
        return;
    }
    rgb = heap_caps_malloc(FRAME_W * FRAME_H * 3, MALLOC_CAP_SPIRAM);
    if (!rgb) {
        ESP_LOGE(TAG, "no PSRAM for the RGB frame");
        return;
    }
#endif
    // Without the hub the on-device screen still works and drives the LED.
    bool online = skincheck_wifi_connect(20000) == ESP_OK;
    if (!online) {
        ESP_LOGW(TAG, "no WiFi -- screening locally only, nothing will be sent");
    }
    led_red(false);

    const float threshold = CONFIG_SKINCHECK_THRESHOLD_PERCENT / 100.f;
    int streak = 0;
    int64_t last_sent_us = -1;
    TickType_t wake = xTaskGetTickCount();
    while (true) {
        if (FLASH_DURING_CAPTURE) {
            gpio_set_level(LED_FLASH_GPIO, 1);
            vTaskDelay(pdMS_TO_TICKS(150));  // let exposure settle
        }
        camera_fb_t *fb = esp_camera_fb_get();
        gpio_set_level(LED_FLASH_GPIO, 0);
        if (!fb) {
            ESP_LOGW(TAG, "capture failed");
        } else {
            float p = (online || SCREEN_ON_DEVICE) ? screen_frame(fb, rgb) : -1.f;
            bool positive = p >= threshold;
            led_red(positive);
            streak = positive ? streak + 1 : 0;
            bool cooled = last_sent_us < 0 ||
                          esp_timer_get_time() - last_sent_us > CONFIG_SKINCHECK_COOLDOWN_S * 1000000LL;
            if (online && streak >= CONFIG_SKINCHECK_POSITIVE_STREAK && cooled) {
                send_for_analysis(fb);
                last_sent_us = esp_timer_get_time();
                streak = 0;
            }
            esp_camera_fb_return(fb);
        }
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(CONFIG_SKINCHECK_CAPTURE_PERIOD_MS));
    }
}
