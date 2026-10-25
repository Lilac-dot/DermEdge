#include "app_sm.h"

#include <string.h>

void sm_init(sm_t *sm) {
    memset(sm, 0, sizeof(*sm));
    sm->state = ST_BOOT;
}

const char *sm_state_name(sm_state_t s) {
    static const char *names[] = {"boot", "pairing", "ready", "recording", "syncing", "error"};
    return s <= ST_ERROR ? names[s] : "?";
}

// Where to rest when not recording
static sm_state_t idle_state(const sm_t *sm) {
    if (!sm->linked) return ST_PAIRING;
    return sm->outbox ? ST_SYNCING : ST_READY;
}

static int go(sm_t *sm, sm_state_t next) {
    if (next == sm->state) return 0;
    sm->state = next;
    sm->page = 0;
    return FX_REDRAW | FX_STATUS;
}

static int start_recording(sm_t *sm, sm_source_t src) {
    sm->recording++;
    sm->source = src;
    return FX_START_REC | go(sm, ST_RECORDING);
}

int sm_handle(sm_t *sm, sm_event_t ev, uint32_t arg) {
    // events that mean the same in every state
    switch (ev) {
    case EV_ALERT:
        sm->alert = true;
        return FX_REDRAW;
    case EV_CMD_DISMISS:
    case EV_DOUBLE_TAP:
        if (!sm->alert) return 0;
        sm->alert = false;
        return FX_REDRAW | (ev == EV_DOUBLE_TAP ? FX_ALERT_DISMISSED | FX_STATUS : 0);
    case EV_PASSKEY:
        sm->passkey = arg;
        return FX_REDRAW;
    case EV_LINK_UP:
        sm->linked = true;
        sm->passkey = 0;
        break;
    case EV_LINK_DOWN:
        sm->linked = false;
        sm->passkey = 0;
        break;
    default:
        break;
    }

    switch (sm->state) {
    case ST_BOOT:
    case ST_ERROR:
        if (ev == EV_SENSOR_OK) return go(sm, idle_state(sm));
        if (ev == EV_SENSOR_FAIL) return go(sm, ST_ERROR);
        return 0;

    case ST_PAIRING:
        if (ev == EV_LINK_UP) return go(sm, idle_state(sm));
        if (ev == EV_LONG_PRESS) return start_recording(sm, SRC_TOUCH);  // offline; syncs later
        if (ev == EV_LINK_DOWN) return FX_REDRAW;
        return 0;

    case ST_READY:
    case ST_SYNCING:
        if (ev == EV_LONG_PRESS) return start_recording(sm, SRC_TOUCH);
        if (ev == EV_CMD_START) return start_recording(sm, SRC_WEBSITE);
        if (ev == EV_LINK_DOWN) return go(sm, ST_PAIRING);
        if (ev == EV_OUTBOX_EMPTY && sm->state == ST_SYNCING) return go(sm, ST_READY);
        if (ev == EV_TAP) {
            sm->page ^= 1;
            return FX_REDRAW;
        }
        return 0;

    case ST_RECORDING:
        if (ev == EV_LONG_PRESS || ev == EV_CMD_STOP) return FX_STOP_REC | go(sm, idle_state(sm));
        if (ev == EV_LINK_UP || ev == EV_LINK_DOWN) return FX_REDRAW | FX_STATUS;  // keeps recording
        if (ev == EV_TAP) {
            sm->page ^= 1;
            return FX_REDRAW;
        }
        return 0;
    }
    return 0;
}
