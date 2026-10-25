// DermEdge band on a plain ESP32: ADXL345 scratch detection, a touch pad, the ST7735 screen and
// a Bluetooth link to the website, run by the state machine in app_sm.c.
//
// A sensor task drains the accelerometer's FIFO every 10 ms (it holds 40 ms at 800 Hz) and a
// detector task scores each second of it; one loop every 20 ms reads the touch pad, handles
// Bluetooth messages and the detector's results, sends queued readings and redraws. Typing
// "raw on" on the USB serial line streams the raw 800 Hz samples for tools/record.py. A skin
// task reads GSR at 4 Hz and the thermistor once a second (skin_adc.c), sent only while they read
// sensibly; "sim on" stands in for them, "skin on" prints the pins, "gsr cal" calibrates. The website relays the
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
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "scratch.h"
#include "skin.h"
#include "skin_adc.h"
#include "touch.h"
#include "ui.h"

static const char *TAG = "band";

#define PIN_TOUCH 27  // the add-on board's touch pad output, high while touched
#define LOOP_MS 20
#define SENSOR_TASK_MS 10  // the FIFO holds 40 ms at 800 Hz
#define SAMPLE_BUFFER (ADXL_HZ * 6 * 2)  // 2 s of raw samples between the sensor and the detector
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
typedef enum { ITEM_MOTION, ITEM_SCRATCH, ITEM_GSR, ITEM_TEMP } item_kind_t;
// 16 bytes, so the outbox still holds an hour of motion; a GSR item packs a second (4 readings).
// With GSR on, about 2.2 items a second: the outbox then covers about 27 minutes offline.
typedef struct {
    uint16_t recording;
    uint8_t kind, n;  // n: GSR readings in v
    float t;          // s since the recording started (GSR: its first reading)
    union {
        struct {
            float a, b;  // motion: activity, scratch_energy; scratch: duration_s, intensity; temp: a = C
        };
        uint16_t v[4];   // GSR in hundredths of a microsiemens, 4 Hz
    };
} item_t;
_Static_assert(sizeof(item_t) == 16, "an item is 16 bytes");

static item_t item(uint32_t rec, item_kind_t kind, float t, float a, float b) {
    return (item_t){.recording = (uint16_t)rec, .kind = kind, .t = t, .a = a, .b = b};
}
static item_t *outbox;  // on the heap: Bluetooth leaves too little static RAM for it
static int ob_len, ob_head, ob_count;

static void ob_alloc(void) {
    for (ob_len = OUTBOX_LEN; ob_len >= 60; ob_len /= 2)
        if ((outbox = malloc(ob_len * sizeof(item_t)))) break;
    ESP_LOGI(TAG, "outbox holds %d readings (%d min offline, about %d with GSR on)", ob_len, ob_len / 60,
             ob_len * 10 / 22 / 60);
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
    switch (it->kind) {
    case ITEM_MOTION:
        snprintf(buf, n, "{\"r\":%u,\"motion\":[{\"t\":%.1f,\"activity\":%.4f,\"scratch_energy\":%.4f}]}",
                 it->recording, it->t, it->a, it->b);
        break;
    case ITEM_SCRATCH:
        snprintf(buf, n, "{\"r\":%u,\"scratch_events\":[{\"t\":%.1f,\"duration_s\":%.1f,\"intensity\":%.4f}]}",
                 it->recording, it->t, it->a, it->b);
        break;
    case ITEM_GSR: {  // {"r":3,"gsr":[{"t":12.25,"microsiemens":1.84},...]}, as docs/DERMEDGE_APP_GUIDE.md says
        int k = snprintf(buf, n, "{\"r\":%u,\"gsr\":[", it->recording);
        for (int i = 0; i < it->n && k < (int)n; i++)
            k += snprintf(buf + k, n - k, "%s{\"t\":%.2f,\"microsiemens\":%.2f}", i ? "," : "",
                          it->t + (float)i / SKIN_GSR_HZ, it->v[i] / 100.0f);
        if (k < (int)n) snprintf(buf + k, n - k, "]}");
        break;
    }
    case ITEM_TEMP:
        snprintf(buf, n, "{\"r\":%u,\"skin_temp\":[{\"t\":%.1f,\"c\":%.2f}]}", it->recording, it->t, it->a);
        break;
    }
}

static void ob_send(void) {
    char buf[200];  // a GSR item is about 145 characters, under the ~180 a notification carries on macOS
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
static touch_t pad;
static uint32_t rec_start_ms, flush_rec;
static char label[24] = "day";

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void send_status(void) {
    char buf[240], gsr[16] = "null", tc[16] = "null";
    if (ui.gsr_ok) snprintf(gsr, sizeof(gsr), "%.2f", ui.gsr_us);
    if (ui.temp_ok) snprintf(tc, sizeof(tc), "%.1f", ui.skin_c);
    // gsr / tc: the live skin readings (null while off skin or unplugged); sim: 1 while simulated
    snprintf(buf, sizeof(buf),
             "{\"id\":\"%s\",\"s\":\"%s\",\"r\":%lu,\"src\":\"%s\",\"label\":\"%s\",\"el\":%.1f,"
             "\"n\":%lu,\"act\":%.3f,\"q\":%d,\"alert\":%d,\"gsr\":%s,\"tc\":%s,\"sim\":%d}",
             ble_device_id(), sm_state_name(sm.state), (unsigned long)sm.recording,
             sm.source == SRC_TOUCH ? "touch" : "website", label,
             sm.state == ST_RECORDING ? ui.elapsed_s : 0.0f, (unsigned long)ui.scratches, ui.activity, ob_count,
             sm.alert, gsr, tc, ui.skin_sim);
    ble_notify_status(buf);
}

// --- skin readings: GSR packed four to an item, temperature averaged over 5 s ---------------
#define TEMP_EVERY_S 5
static QueueHandle_t skin_q;
static item_t gsr_item;  // filling: n readings so far
static float temp_sum, temp_t0;
static int temp_n;

static void skin_flush(void) {
    if (gsr_item.n) ob_push(gsr_item);
    gsr_item.n = 0;
    if (temp_n) ob_push(item(sm.recording, ITEM_TEMP, temp_t0, temp_sum / temp_n, 0));
    temp_n = 0;
    temp_sum = 0;
}

static void skin_results(uint32_t now) {
    skin_reading_t r;
    while (xQueueReceive(skin_q, &r, 0)) {
        ui.gsr_ok = r.gsr_ok, ui.temp_ok = r.temp_ok, ui.skin_sim = r.sim;
        if (r.gsr_ok) ui.gsr_us = r.us;
        if (r.temp_ok) ui.skin_c = r.c;
        if (sm.state != ST_RECORDING) continue;
        float t = (now - rec_start_ms) / 1000.0f;
        if (r.gsr_ok) {
            if (!gsr_item.n) gsr_item = (item_t){.recording = (uint16_t)sm.recording, .kind = ITEM_GSR, .t = t};
            gsr_item.v[gsr_item.n++] = (uint16_t)(r.us * 100 + 0.5f);
            if (gsr_item.n == 4) {
                ob_push(gsr_item);
                gsr_item.n = 0;
            }
        } else if (gsr_item.n) {  // electrodes came off: send what there is, so times stay exact
            ob_push(gsr_item);
            gsr_item.n = 0;
        }
        if (r.temp_ok) {
            if (!temp_n) temp_t0 = t;
            temp_sum += r.c, temp_n++;
            if (t - temp_t0 >= TEMP_EVERY_S) {
                ob_push(item(sm.recording, ITEM_TEMP, temp_t0, temp_sum / temp_n, 0));
                temp_n = 0, temp_sum = 0;
            }
        }
    }
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

// --- sensing: one task drains the ADXL345, another runs the detector ----------------------
enum { OUT_MOTION = 1, OUT_FLUSHED };
typedef struct {
    uint8_t kind;
    bool has_event;
    scratch_motion_t m;
    scratch_event_t e;
} det_out_t;
enum { DET_RESET = 1, DET_FLUSH = 2 };
static StreamBufferHandle_t samples;
static QueueHandle_t det_cmds, det_out;
static volatile bool sensor_ok, raw_mode;
static volatile uint32_t fifo_overruns, buffer_drops;

// raw frame for tools/record.py: A5 5A, count, overrun flag, sample number (LE), count x (x, y, z) int16 LE
static void raw_frame(const int16_t xyz[][3], int n, bool overrun, uint16_t seq) {
    uint8_t hdr[6] = {0xA5, 0x5A, (uint8_t)n, overrun, (uint8_t)(seq & 0xFF), (uint8_t)(seq >> 8)};
    uart_write_bytes(UART_NUM_0, hdr, sizeof(hdr));
    uart_write_bytes(UART_NUM_0, xyz, n * sizeof(xyz[0]));
}

static void sensor_task(void *arg) {
    static int16_t xyz[ADXL_MAX_BATCH][3];
    uint16_t seq = 0;
    uint32_t idle = 0;
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(SENSOR_TASK_MS));
        if (!sensor_ok) {
            if (++idle % (SENSOR_RETRY_MS / SENSOR_TASK_MS) == 0 && adxl_init() == ESP_OK) sensor_ok = true;
            continue;
        }
        bool overrun;
        int n = adxl_read(xyz, ADXL_MAX_BATCH, &overrun);
        if (n < 0) {
            sensor_ok = false;
            continue;
        }
        if (overrun) fifo_overruns++;
        if (!n) continue;
        if (raw_mode) raw_frame(xyz, n, overrun, seq);
        seq += n;
        size_t bytes = n * sizeof(xyz[0]);
        // whole batches only, so the detector never sees half a sample
        if (xStreamBufferSpacesAvailable(samples) >= bytes) xStreamBufferSend(samples, xyz, bytes, 0);
        else buffer_drops++;
    }
}

static void detect_task(void *arg) {
    static scratch_t det;
    static int16_t chunk[64][3];
    scratch_reset(&det);
    for (;;) {
        uint8_t cmd;
        while (xQueueReceive(det_cmds, &cmd, 0)) {
            if (cmd == DET_FLUSH) {
                det_out_t o = {.kind = OUT_FLUSHED};
                o.has_event = scratch_flush(&det, &o.e);
                xQueueSend(det_out, &o, portMAX_DELAY);
            }
            if (cmd == DET_RESET) scratch_reset(&det);
        }
        size_t got = xStreamBufferReceive(samples, chunk, sizeof(chunk), pdMS_TO_TICKS(50));
        for (size_t i = 0; i < got / sizeof(chunk[0]); i++) {
            det_out_t o = {.kind = OUT_MOTION};
            int f = scratch_feed(&det, chunk[i][0] / ADXL_LSB_PER_G, chunk[i][1] / ADXL_LSB_PER_G,
                                 chunk[i][2] / ADXL_LSB_PER_G, &o.m, &o.e);
            if (f & SCRATCH_GOT_MOTION) {
                o.has_event = f & SCRATCH_GOT_EVENT;
                xQueueSend(det_out, &o, 0);  // the UI loop drains it every 20 ms; a full queue just skips a second
            }
        }
    }
}

static int command(char *c);
static int handle(sm_event_t ev, uint32_t arg);
static int serial_dirty;  // a serial command changed the state: redraw

// Last few outbox items, formatted exactly as they'll go out over Bluetooth
static void peek(void) {
    char buf[200];
    int k = ob_count < 6 ? ob_count : 6;
    printf("outbox: %d readings waiting%s\n", ob_count, k ? "; the newest:" : "");
    for (int i = ob_count - k; i < ob_count; i++) {
        ob_format(&outbox[(ob_head + i) % ob_len], buf, sizeof(buf));
        printf("  %s (%d bytes)\n", buf, (int)strlen(buf));
    }
}

// From the USB serial line: "raw on/off" (tools/record.py), "sim on/stress/off", "skin on/off",
// "gsr cal", "peek", and anything the website can send (start, stop, ...)
static void serial_commands(void) {
    static char line[32];
    static int len;
    uint8_t c;
    while (uart_read_bytes(UART_NUM_0, &c, 1, 0) == 1) {
        if (c != '\n' && c != '\r') {
            if (len < (int)sizeof(line) - 1) line[len++] = c;
            continue;
        }
        line[len] = 0;
        len = 0;
        if (!strcmp(line, "raw on")) {
            esp_log_level_set("*", ESP_LOG_NONE);  // nothing but frames on the line
            raw_mode = true;
        } else if (!strcmp(line, "sim on") || !strcmp(line, "sim stress")) {
            skin_set_sim(true);  // fake GSR and temperature, marked SIM on the screen and "sim":1 in the status
            skin_set_sim_stressed(!strcmp(line, "sim stress"));
            ESP_LOGI(TAG, "skin sensors simulated (%s)", strcmp(line, "sim stress") ? "calm" : "stressed");
        } else if (!strcmp(line, "sim off")) {
            skin_set_sim(false);
            ESP_LOGI(TAG, "skin sensors real");
        } else if (!strcmp(line, "skin on") || !strcmp(line, "skin off")) {
            skin_set_debug(!strcmp(line, "skin on"));
        } else if (!strcmp(line, "gsr cal")) {  // with the electrodes off the skin
            float a;
            bool ok = skin_gsr_calibrate(&a);
            if (ok) ESP_LOGI(TAG, "GSR calibrated: no-skin reading %.0f, saved", a);
            else ESP_LOGW(TAG, "GSR calibration refused: reading %.0f is far from 512. Electrodes off? Module wired?", a);
        } else if (!strcmp(line, "raw off")) {
            raw_mode = false;
            esp_log_level_set("*", ESP_LOG_INFO);
            ESP_LOGI(TAG, "raw off; FIFO overruns %lu, buffer drops %lu", (unsigned long)fifo_overruns,
                     (unsigned long)buffer_drops);
        } else if (!strcmp(line, "peek")) {
            peek();
        } else if (!strcmp(line, "start") || !strcmp(line, "stop")) {
            // like the touch pad's long press, so it works without the website too
            bool rec = sm.state == ST_RECORDING;
            if (rec == !strcmp(line, "stop")) serial_dirty |= handle(EV_LONG_PRESS, 0);
        } else if (line[0]) {
            serial_dirty |= command(line);  // dismiss, alert:..., as over Bluetooth
        }
    }
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
        gsr_item.n = 0, temp_n = 0, temp_sum = 0;
        uint8_t c = DET_RESET;
        xQueueSend(det_cmds, &c, portMAX_DELAY);
        rec_start_ms = now_ms();
        ui.scratches = 0;
        ui.elapsed_s = 0;
        if (sm.source == SRC_TOUCH) strlcpy(label, "day", sizeof(label));
    }
    if (fx & FX_STOP_REC) {
        uint8_t c = DET_FLUSH;  // an episode still open comes back as OUT_FLUSHED, for this recording
        flush_rec = sm.recording;
        skin_flush();  // the last part-second of GSR and temperature
        xQueueSend(det_cmds, &c, portMAX_DELAY);
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

static void scratch_found(uint32_t rec, const scratch_event_t *e) {
    ob_push(item(rec, ITEM_SCRATCH, e->t, e->duration_s, e->intensity));
    ui.scratches++;
    ESP_LOGI(TAG, "scratch episode: %.1f s at %.1f s, vibration %.3f g", e->duration_s, e->t, e->intensity);
}

// The detector's results: a summary each second, episodes as they end
static void detector_results(void) {
    det_out_t o;
    while (xQueueReceive(det_out, &o, 0)) {
        bool recording = sm.state == ST_RECORDING;
        if (o.kind == OUT_MOTION) {
            ui.activity = o.m.activity;
            ui.freq_hz = o.m.freq_hz;
            ui.scratchy = o.m.scratchy;
            if (recording) ob_push(item(sm.recording, ITEM_MOTION, o.m.t, o.m.activity, o.m.scratch_energy));
            if (o.has_event && recording) scratch_found(sm.recording, &o.e);
        } else if (o.kind == OUT_FLUSHED && o.has_event) {
            scratch_found(flush_rec, &o.e);  // an episode cut short by stopping
            ui.last_scratches = ui.scratches;
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
    disp_init();
    ui_render(&ui);  // boot screen

    inbox = xQueueCreate(8, sizeof(msg_t));
    ble_start(on_ble);
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 256, 4096, 0, NULL, 0));  // "raw on" in, raw frames out
    samples = xStreamBufferCreate(SAMPLE_BUFFER, 1);
    det_cmds = xQueueCreate(4, sizeof(uint8_t));
    det_out = xQueueCreate(8, sizeof(det_out_t));
    sensor_ok = adxl_init() == ESP_OK;
    bool sensor_was_ok = sensor_ok;
    handle(sensor_ok ? EV_SENSOR_OK : EV_SENSOR_FAIL, 0);
    // the sensor first, on the core NimBLE doesn't use, so nothing delays the FIFO
    xTaskCreatePinnedToCore(sensor_task, "sensor", 4096, NULL, 10, NULL, 1);
    xTaskCreatePinnedToCore(detect_task, "detect", 6144, NULL, 5, NULL, 1);
    skin_q = xQueueCreate(8, sizeof(skin_reading_t));
    skin_adc_start(skin_q);
    ESP_LOGI(TAG, "free heap %lu bytes", (unsigned long)esp_get_free_heap_size());

    uint32_t last_status = 0, last_frame = 0;
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

        serial_commands();
        dirty |= serial_dirty;
        serial_dirty = 0;
        if (sensor_ok != sensor_was_ok) {  // the sensor task lost or found the accelerometer
            sensor_was_ok = sensor_ok;
            if (!sensor_ok) ESP_LOGW(TAG, "accelerometer stopped answering");
            dirty |= handle(sensor_ok ? EV_SENSOR_OK : EV_SENSOR_FAIL, 0);
        }
        detector_results();
        skin_results(now);
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
