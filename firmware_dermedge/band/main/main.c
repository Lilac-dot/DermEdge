// DermEdge band on a plain ESP32: ADXL345 scratch detection, a touch pad, the ST7735 screen and
// a Bluetooth link to the website, run by the state machine in app_sm.c.
//
// One loop every 20 ms reads the touch pad, handles Bluetooth messages, drains the
// accelerometer FIFO every 100 ms, sends queued readings and redraws. The website relays the
// readings to the laptop hub (app/band.py); recordings made offline wait in the outbox (about
// an hour of them) and go out when the laptop reconnects.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adxl345.h"
#include "app_sm.h"
#include "ble.h"
#include "display.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "scratch.h"
#include "touch.h"
#include "ui.h"

static const char *TAG = "band";

#define PIN_TOUCH 27  // the add-on board's touch pad output, high while touched
#define LOOP_MS 20
#define SENSOR_MS 100
#define STATUS_MS 1000
#define FRAME_MS 250      // redraw rate for animated screens
#define SENSOR_RETRY_MS 2000
#define OUTBOX_LEN 3600   // one motion summary a second: an hour offline
#define SENDS_PER_LOOP 6

// --- messages from the Bluetooth task ----------------------------------------------------
typedef struct {
    ble_event_t ev;
    uint32_t arg;
    char text[200];
} msg_t;
static QueueHandle_t inbox;

static void on_ble(ble_event_t ev, uint32_t arg, const char *text) {
    msg_t m = {.ev = ev, .arg = arg};
    if (text) strlcpy(m.text, text, sizeof(m.text));
    xQueueSend(inbox, &m, 0);
}

// --- outbox: readings waiting for the website --------------------------------------------
typedef enum { ITEM_MOTION, ITEM_SCRATCH } item_kind_t;
typedef struct {
    uint16_t recording;
    uint8_t kind;
    float t, a, b;  // motion: t, activity, scratch_energy; scratch: t, duration_s, intensity
} item_t;
static item_t *outbox;  // on the heap: Bluetooth leaves too little static RAM for it
static int ob_len, ob_head, ob_count;

static void ob_alloc(void) {
    for (ob_len = OUTBOX_LEN; ob_len >= 60; ob_len /= 2)
        if ((outbox = malloc(ob_len * sizeof(item_t)))) break;
    ESP_LOGI(TAG, "outbox holds %d readings (%d min offline)", ob_len, ob_len / 60);
}

static void ob_push(item_t it) {
    if (ob_count == ob_len) {  // full: lose the oldest
        ob_head = (ob_head + 1) % ob_len;
        ob_count--;
    }
    outbox[(ob_head + ob_count) % ob_len] = it;
    ob_count++;
}

static void ob_format(const item_t *it, char *buf, size_t n) {
    if (it->kind == ITEM_MOTION)
        snprintf(buf, n, "{\"r\":%u,\"motion\":[{\"t\":%.1f,\"activity\":%.4f,\"scratch_energy\":%.4f}]}",
                 it->recording, it->t, it->a, it->b);
    else
        snprintf(buf, n, "{\"r\":%u,\"scratch_events\":[{\"t\":%.1f,\"duration_s\":%.1f,\"intensity\":%.4f}]}",
                 it->recording, it->t, it->a, it->b);
}

static void ob_send(void) {
    char buf[160];
    for (int i = 0; i < SENDS_PER_LOOP && ob_count && ble_data_ready(); i++) {
        ob_format(&outbox[ob_head], buf, sizeof(buf));
        if (!ble_notify_data(buf)) break;  // radio busy: try next loop
        ob_head = (ob_head + 1) % ob_len;
        ob_count--;
    }
}

// --- state -------------------------------------------------------------------------------
static sm_t sm;
static ui_model_t ui = {.sm = &sm};
static scratch_t det;
static touch_t pad;
static bool sensor_ok;
static uint32_t rec_start_ms;
static char label[24] = "day";

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void send_status(void) {
    char buf[200];
    snprintf(buf, sizeof(buf),
             "{\"id\":\"%s\",\"s\":\"%s\",\"r\":%lu,\"src\":\"%s\",\"label\":\"%s\",\"el\":%.1f,"
             "\"n\":%lu,\"act\":%.3f,\"q\":%d,\"alert\":%d}",
             ble_device_id(), sm_state_name(sm.state), (unsigned long)sm.recording,
             sm.source == SRC_TOUCH ? "touch" : "website", label,
             sm.state == ST_RECORDING ? ui.elapsed_s : 0.0f, (unsigned long)ui.scratches, ui.activity, ob_count,
             sm.alert);
    ble_notify_status(buf);
}

static void save_token(const char *token) {
    nvs_handle_t h;
    if (nvs_open("band", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "token", token);  // for posting straight to the hub over Wi-Fi, later
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "device token saved");
}

// Copies "a|b|c" fields into the alert texts
static void parse_alert(char *s) {
    char *f[3] = {s, "", ""};
    for (int i = 1; i < 3; i++) {
        char *bar = strchr(f[i - 1], '|');
        if (!bar) break;
        *bar = 0;
        f[i] = bar + 1;
    }
    strlcpy(ui.alert_level, f[0], sizeof(ui.alert_level));
    strlcpy(ui.alert_reason, f[1], sizeof(ui.alert_reason));
    strlcpy(ui.alert_tip, f[2], sizeof(ui.alert_tip));
}

static int apply(int fx);

static int handle(sm_event_t ev, uint32_t arg) {
    sm.outbox = ob_count;
    int fx = sm_handle(&sm, ev, arg);
    if (fx) ESP_LOGI(TAG, "event %d -> %s", ev, sm_state_name(sm.state));
    return apply(fx);
}

static int command(char *c) {
    ESP_LOGI(TAG, "command: %s", c);
    if (!strncmp(c, "start", 5)) {
        strlcpy(label, c[5] == ':' && c[6] ? c + 6 : "day", sizeof(label));
        return handle(EV_CMD_START, 0);
    }
    if (!strcmp(c, "stop")) return handle(EV_CMD_STOP, 0);
    if (!strcmp(c, "dismiss")) return handle(EV_CMD_DISMISS, 0);
    if (!strncmp(c, "alert:", 6)) {
        parse_alert(c + 6);
        return handle(EV_ALERT, 0);
    }
    if (!strncmp(c, "token:", 6)) save_token(c + 6);
    if (!strcmp(c, "unpair")) ble_forget_bonds();
    return 0;
}

// Carries out the state machine's effects; returns whether to redraw
static int apply(int fx) {
    if (fx & FX_START_REC) {
        scratch_reset(&det);
        rec_start_ms = now_ms();
        ui.scratches = 0;
        ui.elapsed_s = 0;
        if (sm.source == SRC_TOUCH) strlcpy(label, "day", sizeof(label));
    }
    if (fx & FX_STOP_REC) {
        scratch_event_t e;
        if (scratch_flush(&det, &e)) {
            ob_push((item_t){(uint16_t)sm.recording, ITEM_SCRATCH, e.t, e.duration_s, e.intensity});
            ui.scratches++;
        }
        ui.has_last = true;
        ui.last_minutes = ui.elapsed_s / 60;
        ui.last_scratches = ui.scratches;
        // where to rest depends on what's still queued
        sm.outbox = ob_count;
        if (sm.state == ST_READY && ob_count) sm.state = ST_SYNCING;
    }
    if (fx & FX_STATUS) send_status();
    return fx & FX_REDRAW;
}

static void read_sensor(void) {
    static float xyz[ADXL_MAX_BATCH][3];
    int n = adxl_read(xyz, ADXL_MAX_BATCH);
    if (n < 0) {
        ESP_LOGW(TAG, "accelerometer stopped answering");
        sensor_ok = false;
        handle(EV_SENSOR_FAIL, 0);
        return;
    }
    bool recording = sm.state == ST_RECORDING;
    for (int i = 0; i < n; i++) {
        scratch_motion_t m;
        scratch_event_t e;
        int got = scratch_feed(&det, xyz[i][0], xyz[i][1], xyz[i][2], &m, &e);
        if (got & SCRATCH_GOT_MOTION) {
            ui.activity = m.activity;
            ui.freq_hz = m.freq_hz;
            ui.scratchy = m.scratchy;
            if (recording) ob_push((item_t){(uint16_t)sm.recording, ITEM_MOTION, m.t, m.activity, m.scratch_energy});
        }
        if (got & SCRATCH_GOT_EVENT && recording) {
            ob_push((item_t){(uint16_t)sm.recording, ITEM_SCRATCH, e.t, e.duration_s, e.intensity});
            ui.scratches++;
            ESP_LOGI(TAG, "scratch episode: %.1f s at %.1f s, intensity %.2f g", e.duration_s, e.t, e.intensity);
        }
    }
}

void app_main(void) {
    esp_err_t err = nvs_flash_init();  // Bluetooth bonds live here
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    gpio_config_t io = {.pin_bit_mask = 1ULL << PIN_TOUCH, .mode = GPIO_MODE_INPUT,
                        .pull_down_en = GPIO_PULLDOWN_ENABLE};  // reads "not touched" if unplugged
    ESP_ERROR_CHECK(gpio_config(&io));

    ob_alloc();
    sm_init(&sm);
    pad_init(&pad);
    scratch_reset(&det);
    disp_init();
    ui_render(&ui);  // boot screen

    inbox = xQueueCreate(8, sizeof(msg_t));
    ble_start(on_ble);
    sensor_ok = adxl_init() == ESP_OK;
    handle(sensor_ok ? EV_SENSOR_OK : EV_SENSOR_FAIL, 0);

    uint32_t last_sensor = 0, last_status = 0, last_frame = 0, last_retry = now_ms();
    bool dirty = true;
    while (1) {
        uint32_t now = now_ms();
        ui.now_ms = now;

        switch (pad_feed(&pad, gpio_get_level(PIN_TOUCH), now)) {
        case TOUCH_TAP: dirty |= handle(EV_TAP, 0); break;
        case TOUCH_DOUBLE_TAP: dirty |= handle(EV_DOUBLE_TAP, 0); break;
        case TOUCH_LONG_PRESS: dirty |= handle(EV_LONG_PRESS, 0); break;
        default: break;
        }
        uint32_t held = pad_held_ms(&pad, now);
        if (held / 100 != ui.held_ms / 100) dirty = true;  // animate the hold bar
        ui.held_ms = held > 150 ? held : 0;  // a tap shouldn't flash the bar

        msg_t m;
        while (xQueueReceive(inbox, &m, 0)) {
            switch (m.ev) {
            case BLE_EV_LINK_UP: dirty |= handle(EV_LINK_UP, 0); break;
            case BLE_EV_LINK_DOWN: dirty |= handle(EV_LINK_DOWN, 0); break;
            case BLE_EV_PASSKEY: dirty |= handle(EV_PASSKEY, m.arg); break;
            case BLE_EV_COMMAND: dirty |= command(m.text); break;
            }
        }

        if (sensor_ok && now - last_sensor >= SENSOR_MS) {
            last_sensor = now;
            read_sensor();
        } else if (!sensor_ok && now - last_retry >= SENSOR_RETRY_MS) {
            last_retry = now;
            if (adxl_init() == ESP_OK) {
                sensor_ok = true;
                dirty |= handle(EV_SENSOR_OK, 0);
            }
        }
        if (sm.state == ST_RECORDING) ui.elapsed_s = (now - rec_start_ms) / 1000.0f;

        ob_send();
        sm.outbox = ob_count;
        if (sm.state == ST_SYNCING && !ob_count) dirty |= handle(EV_OUTBOX_EMPTY, 0);

        if (now - last_status >= STATUS_MS) {
            last_status = now;
            send_status();
        }
        bool animated = sm.state == ST_RECORDING || sm.state == ST_SYNCING;
        if (dirty || (animated && now - last_frame >= FRAME_MS)) {
            last_frame = now;
            dirty = false;
            ui_render(&ui);
        }
        vTaskDelay(pdMS_TO_TICKS(LOOP_MS));
    }
}
