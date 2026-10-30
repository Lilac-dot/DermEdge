// Which screen the band shows, and which warning: a small state machine on top of the device's
// (app_sm.c), so a recording can be confirmed, a hold shows its progress, and a stop shows what
// was saved. Plain C, so the host tests walk it.
//
//   holding the pad (> 150 ms) ............ HOLD_START / HOLD_STOP, a ring filling over 2 s
//   a recording starts ..................... STARTED for 1.5 s, then RECORDING
//   a recording stops ...................... SAVED for 4 s (time, scratch episodes, synced?)
//   a tap on HOME or RECORDING ............. SENSORS (plain-language status), tap again to go back
//   otherwise .............................. the device state's screen
// Warnings sit in a strip along the bottom, most urgent first: band memory nearly full,
// temperature sensor not reading (only once it had worked, for 10 s), recording while not
// connected. ECG electrodes coming off doesn't warn: the heart check is a few minutes sitting
// still, and the electrodes are meant to come off afterwards.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "app_sm.h"

#define UI_HOLD_SHOW_MS 150    // a tap shouldn't flash the hold screen
#define UI_STARTED_MS 1500
#define UI_SAVED_MS 4000
#define UI_SENSOR_GRACE_MS 10000
#define UI_STORAGE_WARN 0.8f   // fraction of the outbox

typedef enum {
    SCR_BOOT, SCR_CONNECT, SCR_HOME, SCR_RECORDING, SCR_SYNCING, SCR_SENSOR_ERROR,
    SCR_HOLD_START, SCR_HOLD_STOP, SCR_STARTED, SCR_SAVED, SCR_SENSORS,
} ui_screen_t;

typedef enum { WARN_NONE, WARN_STORAGE, WARN_TEMP_OFF, WARN_OFFLINE } ui_warning_t;

typedef enum { SENSOR_ABSENT, SENSOR_IDLE, SENSOR_OK } ui_sensor_t;  // not connected / connected, not reading / ok

typedef struct {
    sm_state_t state;
    bool linked;
    int page;                 // app_sm's tap toggle: 1 = the sensors page
    uint32_t held_ms;         // touch pad held, 0 if not
    uint32_t outbox, outbox_cap;
    ui_sensor_t temp;
    uint32_t now_ms;
} ui_inputs_t;

typedef struct {
    sm_state_t last_state;
    uint32_t started_until, saved_until;
    uint32_t temp_bad_since;  // 0: reading fine (or never worked)
    bool temp_worked;
} ui_flow_t;

void ui_flow_init(ui_flow_t *f);
// Call every frame with the latest inputs; returns the screen to draw
ui_screen_t ui_flow_screen(ui_flow_t *f, const ui_inputs_t *in);
ui_warning_t ui_flow_warning(const ui_flow_t *f, const ui_inputs_t *in);
// Progress of a hold, 0..1
float ui_flow_hold_progress(const ui_inputs_t *in);
