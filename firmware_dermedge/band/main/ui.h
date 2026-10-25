// The band's screens in the website's teal theme (web/index.html), chosen by ui_flow.c's state
// machine: Home, hold-to-start/stop, Started, Recording, Saved, Sensors, Connect, Syncing, the
// sensor-error screen, a flare-alert card on top, and a warning strip along the bottom.
// Users see plain status, not raw numbers; those are on the USB serial line ("skin on").
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "app_sm.h"
#include "ui_flow.h"

typedef struct {
    const sm_t *sm;
    uint32_t now_ms;
    float elapsed_s;          // current recording
    uint32_t scratches;       // episodes this recording
    float activity, freq_hz;  // last second (diagnostics only)
    bool scratchy;
    uint32_t held_ms;         // touch pad held
    float last_minutes;       // the last finished recording
    uint32_t last_scratches;
    bool has_last;
    char alert_level[12], alert_reason[64], alert_tip[120];
    float gsr_us, skin_c;     // latest skin readings (status only, not drawn)
    bool gsr_ok, temp_ok;     // on skin / plugged in
    int skin_sim;             // 1 while the skin sensors are simulated
    ui_sensor_t gsr_state, temp_state;
    uint32_t outbox_cap;
    int preview_screen;       // test/preview.c only: draw this screen + 1 (0 = follow ui_flow)
    int preview_warning;      // likewise, warning + 1
} ui_model_t;

void ui_render(const ui_model_t *m);
