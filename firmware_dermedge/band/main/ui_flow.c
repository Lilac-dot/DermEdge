#include "ui_flow.h"

#include <string.h>

#include "touch.h"

void ui_flow_init(ui_flow_t *f) {
    memset(f, 0, sizeof(*f));
    f->last_state = ST_BOOT;
}

// A sensor that worked and then stopped reading for UI_SENSOR_GRACE_MS (0 = fine)
static void track(ui_sensor_t s, bool *worked, uint32_t *bad_since, uint32_t now) {
    if (s == SENSOR_OK) {
        *worked = true;
        *bad_since = 0;
    } else if (*worked && !*bad_since) {
        *bad_since = now ? now : 1;
    }
}

ui_screen_t ui_flow_screen(ui_flow_t *f, const ui_inputs_t *in) {
    uint32_t now = in->now_ms;
    // transitions of the device state start the confirmation screens
    if (in->state == ST_RECORDING && f->last_state != ST_RECORDING) {
        f->started_until = now + UI_STARTED_MS;
        f->saved_until = 0;
        f->temp_bad_since = 0;
    }
    if (f->last_state == ST_RECORDING && in->state != ST_RECORDING) {
        f->saved_until = now + UI_SAVED_MS;
        f->started_until = 0;
    }
    f->last_state = in->state;
    track(in->temp, &f->temp_worked, &f->temp_bad_since, now);

    switch (in->state) {
    case ST_BOOT: return SCR_BOOT;
    case ST_ERROR: return SCR_SENSOR_ERROR;
    default: break;
    }
    // a hold in progress shows what it will do (a long press works in every other state)
    if (in->held_ms > UI_HOLD_SHOW_MS && in->held_ms < TOUCH_LONG_MS)
        return in->state == ST_RECORDING ? SCR_HOLD_STOP : SCR_HOLD_START;
    if (in->state == ST_RECORDING && now < f->started_until) return SCR_STARTED;
    if (in->state != ST_RECORDING && now < f->saved_until) return SCR_SAVED;
    if (in->page && (in->state == ST_READY || in->state == ST_RECORDING || in->state == ST_SYNCING)) return SCR_SENSORS;
    switch (in->state) {
    case ST_PAIRING: return SCR_CONNECT;
    case ST_READY: return SCR_HOME;
    case ST_RECORDING: return SCR_RECORDING;
    case ST_SYNCING: return SCR_SYNCING;
    default: return SCR_HOME;
    }
}

ui_warning_t ui_flow_warning(const ui_flow_t *f, const ui_inputs_t *in) {
    uint32_t now = in->now_ms;
    if (in->state == ST_BOOT || in->state == ST_ERROR) return WARN_NONE;
    if (in->outbox_cap && in->outbox >= UI_STORAGE_WARN * in->outbox_cap) return WARN_STORAGE;
    if (in->state != ST_RECORDING) return WARN_NONE;
    if (f->temp_bad_since && now - f->temp_bad_since >= UI_SENSOR_GRACE_MS) return WARN_TEMP_OFF;
    if (!in->linked) return WARN_OFFLINE;
    return WARN_NONE;
}

float ui_flow_hold_progress(const ui_inputs_t *in) {
    float p = (float)in->held_ms / TOUCH_LONG_MS;
    return p < 0 ? 0 : p > 1 ? 1 : p;
}
