// The band's state machine. Plain C: it takes events and returns effects for main.c to carry
// out (start the recorder, redraw, tell the website), so the host tests can walk every path.
//
//   BOOT ──sensor ok──► PAIRING ──secure link──► READY ──start──► RECORDING ──stop──► SYNCING ──► READY
//     │                   ▲  │ long press (offline recording)        │ link lost: keeps recording
//     └─sensor fail─► ERROR   └──────────────────────────────────────►┘
//
// "start"/"stop" come from the website over Bluetooth or a 2 s press on the touch pad. A flare
// alert from the website sits on top of any state until a double tap or the website dismisses it.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { ST_BOOT, ST_PAIRING, ST_READY, ST_RECORDING, ST_SYNCING, ST_ERROR } sm_state_t;

typedef enum {
    EV_SENSOR_OK,
    EV_SENSOR_FAIL,
    EV_LINK_UP,       // encrypted, paired Bluetooth link
    EV_LINK_DOWN,
    EV_PASSKEY,       // arg: the 6-digit code to show
    EV_CMD_START,     // from the website
    EV_CMD_STOP,
    EV_CMD_DISMISS,
    EV_ALERT,         // the website sent a flare alert
    EV_TAP,
    EV_DOUBLE_TAP,
    EV_LONG_PRESS,
    EV_OUTBOX_EMPTY,  // everything recorded has reached the website
} sm_event_t;

enum {
    FX_REDRAW = 1 << 0,
    FX_STATUS = 1 << 1,           // send the status to the website
    FX_START_REC = 1 << 2,
    FX_STOP_REC = 1 << 3,
    FX_ALERT_DISMISSED = 1 << 4,  // tell the website the alert was seen
};

typedef enum { SRC_WEBSITE, SRC_TOUCH } sm_source_t;

typedef struct {
    sm_state_t state;
    bool linked, alert;
    uint32_t passkey;     // shown while pairing, 0 = none
    uint32_t recording;   // counts recordings since boot; tags their readings
    sm_source_t source;   // who started the current recording
    int page;             // tap flips between the main and detail view
    uint32_t outbox;      // readings not yet sent; main.c keeps this current
} sm_t;

void sm_init(sm_t *sm);
int sm_handle(sm_t *sm, sm_event_t ev, uint32_t arg);
const char *sm_state_name(sm_state_t s);
