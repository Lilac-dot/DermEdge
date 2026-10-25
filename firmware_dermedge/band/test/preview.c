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

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    sm_t sm = {0};
    ui_model_t m = {.sm = &sm, .now_ms = 0};

    sm.state = ST_BOOT;
    ui_render(&m);
    save(dir, "1_boot");

    sm.state = ST_PAIRING;
    ui_render(&m);
    save(dir, "2_pairing");
    sm.passkey = 482913;
    ui_render(&m);
    save(dir, "3_pairing_code");

    sm.passkey = 0;
    sm.linked = true;
    sm.state = ST_READY;
    m.has_last = true;
    m.last_minutes = 42;
    m.last_scratches = 3;
    m.held_ms = 1200;
    ui_render(&m);
    save(dir, "4_ready_holding");

    m.held_ms = 0;
    sm.state = ST_RECORDING;
    sm.source = SRC_TOUCH;
    m.elapsed_s = 754;
    m.activity = 0.31f;
    m.freq_hz = 5.2f;
    m.scratchy = true;
    m.scratches = 2;
    sm.outbox = 14;
    ui_render(&m);
    save(dir, "5_recording");
    sm.page = 1;
    ui_render(&m);
    save(dir, "6_recording_detail");

    sm.page = 0;
    sm.state = ST_SYNCING;
    sm.outbox = 37;
    m.now_ms = 300;
    ui_render(&m);
    save(dir, "7_syncing");

    sm.state = ST_ERROR;
    ui_render(&m);
    save(dir, "8_error");

    sm.state = ST_READY;
    sm.alert = true;
    strcpy(m.alert_level, "high");
    strcpy(m.alert_reason, "Night scratching and stress are both up.");
    strcpy(m.alert_tip, "Take a 5-minute breathing break and keep nails short.");
    ui_render(&m);
    save(dir, "9_alert");
    return 0;
}
