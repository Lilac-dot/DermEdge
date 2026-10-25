// The band's screens, one per state plus the alert card on top, in the website's petal and
// sage palette (web/index.html, light theme).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "app_sm.h"

typedef struct {
    const sm_t *sm;
    uint32_t now_ms;
    float elapsed_s;          // current recording
    uint32_t scratches;       // episodes this recording
    float activity, freq_hz;  // last second
    bool scratchy;
    uint32_t held_ms;         // touch pad held, for the hold-to-start bar
    float last_minutes;       // the last finished recording
    uint32_t last_scratches;
    bool has_last;
    char alert_level[12], alert_reason[64], alert_tip[120];
} ui_model_t;

void ui_render(const ui_model_t *m);
