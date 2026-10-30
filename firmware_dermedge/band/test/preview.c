// Renders every band screen with the real ui.c and fonts into PPM images, so the design can be
// checked on the Mac without flashing:  sh test/preview.sh  (writes test/preview.png)
#include <stdio.h>
#include <string.h>

#include "../main/display.h"
#include "../main/ui.h"

void disp_init(void) {}
void disp_flush(void) {}

static void save(const char *dir, const char *name) {
    char path[256];
    snprintf(path, sizeof(path), "%s/%s.ppm", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6 %d %d 255\n", DISP_W, DISP_H);
    const uint16_t *fb = disp_framebuffer();
    for (int i = 0; i < DISP_W * DISP_H; i++) {
        uint16_t c = (uint16_t)(fb[i] >> 8 | fb[i] << 8);  // stored big-endian for the panel
        unsigned char rgb[3] = {(unsigned char)((c >> 11) * 255 / 31), (unsigned char)(((c >> 5) & 63) * 255 / 63),
                                (unsigned char)((c & 31) * 255 / 31)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static sm_t sm;
static ui_model_t m;

// Draws `screen` (with an optional warning) in device state `state`, and saves it as `name`
static void shot(const char *dir, const char *name, sm_state_t state, ui_screen_t screen, ui_warning_t warn) {
    sm.state = state;
    m.preview_screen = screen + 1;
    m.preview_warning = warn + 1;
    ui_render(&m);
    save(dir, name);
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    m = (ui_model_t){.sm = &sm, .outbox_cap = 3600, .ecg_state = SENSOR_OK, .hr_bpm = 71, .temp_state = SENSOR_OK};

    shot(dir, "01_boot", ST_BOOT, SCR_BOOT, WARN_NONE);
    shot(dir, "02_connect", ST_PAIRING, SCR_CONNECT, WARN_NONE);
    sm.passkey = 482913;
    shot(dir, "03_pairing_code", ST_PAIRING, SCR_CONNECT, WARN_NONE);
    sm.passkey = 0;
    sm.linked = true;
    m.has_last = true, m.last_minutes = 42, m.last_scratches = 3;
    shot(dir, "04_home", ST_READY, SCR_HOME, WARN_NONE);
    m.held_ms = 1300;
    shot(dir, "05_hold_to_start", ST_READY, SCR_HOLD_START, WARN_NONE);
    m.held_ms = 0;
    shot(dir, "06_started", ST_RECORDING, SCR_STARTED, WARN_NONE);
    m.elapsed_s = 754, m.scratches = 2, m.now_ms = 0;
    shot(dir, "07_recording", ST_RECORDING, SCR_RECORDING, WARN_NONE);
    m.scratchy = true;
    shot(dir, "08_recording_scratching", ST_RECORDING, SCR_RECORDING, WARN_NONE);
    m.scratchy = false, m.temp_state = SENSOR_IDLE;
    shot(dir, "09_warning_temp_sensor", ST_RECORDING, SCR_RECORDING, WARN_TEMP_OFF);
    m.temp_state = SENSOR_OK, sm.linked = false;
    shot(dir, "10_warning_offline", ST_RECORDING, SCR_RECORDING, WARN_OFFLINE);
    sm.outbox = 3100;
    shot(dir, "11_warning_memory", ST_RECORDING, SCR_RECORDING, WARN_STORAGE);
    sm.outbox = 3560;  // 98.9%: only scratch episodes are stored now
    shot(dir, "11b_warning_memory_full", ST_RECORDING, SCR_RECORDING, WARN_STORAGE);
    sm.outbox = 412, sm.linked = true, m.ecg_state = SENSOR_IDLE, m.hr_bpm = 0;
    shot(dir, "12_sensors", ST_RECORDING, SCR_SENSORS, WARN_NONE);
    m.held_ms = 700;
    shot(dir, "13_hold_to_stop", ST_RECORDING, SCR_HOLD_STOP, WARN_NONE);
    m.held_ms = 0, m.last_minutes = 13, m.last_scratches = 2;
    shot(dir, "14_saved", ST_SYNCING, SCR_SAVED, WARN_NONE);
    sm.outbox = 37, m.now_ms = 300;
    shot(dir, "15_syncing", ST_SYNCING, SCR_SYNCING, WARN_NONE);
    shot(dir, "16_sensor_error", ST_ERROR, SCR_SENSOR_ERROR, WARN_NONE);
    sm.outbox = 0, sm.alert = true;
    strcpy(m.alert_level, "high");
    strcpy(m.alert_reason, "Scratching and stress are both up.");
    strcpy(m.alert_tip, "Try a cool compress, and a 5-minute breathing break.");
    shot(dir, "17_flare_alert", ST_READY, SCR_HOME, WARN_NONE);
    return 0;
}
