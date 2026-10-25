#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "art.h"
#include "display.h"
#include "fonts.h"
#include "touch.h"

// The website's teal theme (web/index.html, light). Its pale tints wash out on this panel, so
// pale ones are a step deeper: --accent-soft #dcefec becomes C_SOFT.
#define C_BG RGB(0x0F, 0x7C, 0x74)     // --accent, teal: the screen behind the cards
#define C_DEEP RGB(0x13, 0x4E, 0x4A)   // --deep: the header
#define C_CARD RGB(0xFF, 0xFF, 0xFF)   // --card
#define C_TEAL RGB(0x0F, 0x7C, 0x74)   // titles on cards
#define C_SOFT RGB(0xC4, 0xE2, 0xDD)   // tracks, the logo's circle
#define C_TEXT RGB(0x1D, 0x2B, 0x2A)   // --text
#define C_MUTED RGB(0x5C, 0x6F, 0x6C)  // --muted
#define C_OK RGB(0x3A, 0x7D, 0x1F)     // --ok, green
#define C_WARN RGB(0xC5, 0x6A, 0x1A)   // --warn
#define C_BAD RGB(0xC9, 0x2F, 0x4C)    // --bad
#define C_WHITE RGB(0xFF, 0xFF, 0xFF)

// Pixel art: '#' first colour, '+' second, anything else transparent
static const char *BLUETOOTH[] = {
    "...#...", "...##..", "#..#.#.", ".#.#..#", "..##.#.", "...##..",
    "..##.#.", ".#.#..#", "#..#.#.", "...##..", "...#...",
};
static const char *FLOWER[] = {
    "..++.++..", ".+++++++.", "+++###+++", ".++###++.", "+++###+++", ".+++++++.", "..++.++..", "....#....", "...##....",
};
static const char *SYNC[] = {
    "..#......", ".##......", "#########", ".##......", "..#......",
    "......#..", "......##.", "#########", "......##.", "......#..",
};
static const char *WARN[] = {
    "....#....", "...#+#...", "...#+#...", "..##+##..", "..##+##..", ".#######.", ".###+###.", "#########",
};
static const char *CLAW[] = {"#..#..#..", "#..#..#..", ".#..#..#.", ".#..#..#.", "..#..#..#", "..#..#..#"};
static const char *CHECK[] = {"........##", ".......##.", "......##..", "##...##...", ".##.##....", "..###.....", "...#......"};
static const char *HAND[] = {  // a fingertip on the pad
    "...##....", "...##....", "...##....", "...#####.", "...######", "#..######", "##.######", ".########",
    "..######.", "..######.", "...####..",
};

#define ROWS(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define ICON(x, y, art, scale, c1, c2) icon(x, y, art, ROWS(art), scale, c1, c2)

static void icon(int x, int y, const char **rows, int n, int scale, uint16_t c1, uint16_t c2) {
    for (int j = 0; j < n; j++)
        for (int i = 0; rows[j][i]; i++) {
            char p = rows[j][i];
            if (p == '#' || p == '+') disp_rect(x + i * scale, y + j * scale, scale, scale, p == '#' ? c1 : c2);
        }
}

static void logo(const mask_t *circle, const mask_t *drop, const mask_t *shine, int x, int y, uint16_t circle_c) {
    disp_mask(circle, x, y, circle_c);
    disp_mask(drop, x, y, C_TEAL);
    disp_mask(shine, x, y, C_WHITE);
}

#define CARD_X 6
#define CARD_Y 22
#define CARD_W 148
#define IN_X (CARD_X + 9)
#define IN_W (CARD_W - 18)
#define IN_R (CARD_X + CARD_W - 9)
#define FULL_B 122  // card bottom without a warning
#define WARN_Y 107  // the warning strip

static int card_b;  // this frame's card bottom

static void header(const ui_model_t *m) {
    disp_rect(0, 0, DISP_W, 18, C_DEEP);
    logo(&logo_sm_circle, &logo_sm_drop, &logo_sm_shine, 4, 2, C_SOFT);
    disp_ftext(&font_label, 22, 3, "DermEdge", C_WHITE);
    if (m->skin_sim) {  // never let simulated skin readings pass for real ones
        disp_round_rect(84, 3, 26, 13, 4, C_WHITE);
        disp_ftext(&font_label, 87, 3, "SIM", C_BAD);
    }
    if (m->sm->state == ST_RECORDING && (m->now_ms / 500) % 2 == 0) {  // blinking REC
        disp_circle(117, 10, 3, C_BAD);
        disp_ftext(&font_label, 123, 3, "REC", C_WHITE);
    }
    ICON(148, 4, BLUETOOTH, 1, m->sm->linked ? C_WHITE : C_SOFT, 0);
}

static void card(void) {
    disp_round_rect(CARD_X, CARD_Y, CARD_W, card_b - CARD_Y, 8, C_CARD);
}

static void title(const char *s, uint16_t c) {
    disp_ftext(&font_title, IN_X, CARD_Y + 3, s, c);
}

// a coloured dot and a line of text: one status line
static void status_line(int y, bool good, const char *text) {
    disp_circle(IN_X + 3, y + 7, 3, good ? C_OK : C_SOFT);
    disp_ftext(&font_body, IN_X + 12, y, text, good ? C_TEXT : C_MUTED);
}

// "label ........ value" in plain words
static void row(int y, const char *label, const char *value, uint16_t vc) {
    disp_ftext(&font_body, IN_X, y, label, C_MUTED);
    disp_ftext(&font_body, IN_R - disp_ftext_width(&font_body, value), y, value, vc);
}

static void footer(const char *s, uint16_t c) {
    disp_ftext(&font_label, IN_X, card_b - 15, s, c);
}

static const char *plural(uint32_t n) { return n == 1 ? "" : "s"; }

// --- screens -----------------------------------------------------------------------------
static void screen_boot(void) {
    disp_fill(C_BG);
    logo(&logo_lg_circle, &logo_lg_drop, &logo_lg_shine, (DISP_W - 56) / 2, 12, C_SOFT);
    disp_ftext_centered(&font_title, DISP_W / 2, 72, "DermEdge", C_WHITE);
    disp_ftext_centered(&font_label, DISP_W / 2, 100, "starting...", C_SOFT);
}

static void screen_connect(const ui_model_t *m) {
    card();
    title("Connect", C_TEAL);
    ICON(IN_R - 14, CARD_Y + 8, BLUETOOTH, 2, C_TEAL, 0);
    char buf[40];
    if (m->sm->passkey) {
        snprintf(buf, sizeof(buf), "%03lu %03lu", (unsigned long)m->sm->passkey / 1000, (unsigned long)m->sm->passkey % 1000);
        disp_ftext(&font_label, IN_X, CARD_Y + 31, "Pairing code", C_MUTED);
        disp_ftext_centered(&font_big, DISP_W / 2, CARD_Y + 38, buf, C_TEXT);
        disp_ftext_centered(&font_body, DISP_W / 2, CARD_Y + 76, "Type it on your phone", C_MUTED);
        return;
    }
    int y = disp_ftext_wrapped(&font_body, IN_X, CARD_Y + 32, IN_W, 0, "Open DermEdge and tap Connect band", C_TEXT);
    if (m->sm->outbox) {
        snprintf(buf, sizeof(buf), "%lu readings to sync", (unsigned long)m->sm->outbox);
        disp_ftext(&font_label, IN_X, y + 2, buf, C_TEAL);
    }
    footer("Hold the pad to record", C_MUTED);
}

static void screen_home(const ui_model_t *m) {
    card();
    logo(&logo_md_circle, &logo_md_drop, &logo_md_shine, IN_X - 2, CARD_Y + 5, C_SOFT);
    disp_ftext(&font_title, IN_X + 34, CARD_Y + 8, "Ready", C_TEAL);
    status_line(CARD_Y + 40, m->sm->linked, m->sm->linked ? "Phone connected" : "Phone not connected");
    status_line(CARD_Y + 54, m->gsr_state == SENSOR_OK,
                m->gsr_state == SENSOR_OK ? "Skin sensor on" : m->gsr_state == SENSOR_IDLE ? "Skin sensor off skin" : "No skin sensor");
    if (m->has_last && card_b == FULL_B) {
        char buf[40];
        snprintf(buf, sizeof(buf), "Last: %.0f min, %lu scratch%s", m->last_minutes, (unsigned long)m->last_scratches,
                 m->last_scratches == 1 ? "" : "es");
        disp_ftext(&font_label, IN_X, CARD_Y + 70, buf, C_MUTED);
    }
    ICON(IN_X, card_b - 17, HAND, 1, C_TEAL, 0);
    disp_ftext(&font_label, IN_X + 13, card_b - 15, "Hold the pad to start", C_TEAL);
}

static void screen_hold(const ui_model_t *m, bool stop) {
    card();
    int cy = CARD_Y + 34;
    disp_ring(DISP_W / 2, cy, 23, 17, ui_flow_hold_progress(&(ui_inputs_t){.held_ms = m->held_ms}), stop ? C_BAD : C_TEAL, C_SOFT);
    ICON(DISP_W / 2 - 4, cy - 6, HAND, 1, stop ? C_BAD : C_TEAL, 0);
    disp_ftext_centered(&font_body, DISP_W / 2, CARD_Y + 61, stop ? "Keep holding to stop" : "Keep holding to start", C_TEXT);
    disp_ftext_centered(&font_label, DISP_W / 2, CARD_Y + 76, "Let go to cancel", C_MUTED);
}

static void screen_started(void) {
    card();
    disp_circle(DISP_W / 2, CARD_Y + 30, 17, C_OK);
    ICON(DISP_W / 2 - 10, CARD_Y + 23, CHECK, 2, C_WHITE, 0);
    disp_ftext_centered(&font_title, DISP_W / 2, CARD_Y + 52, "Recording", C_TEAL);
    disp_ftext_centered(&font_label, DISP_W / 2, CARD_Y + 78, "Hold the pad again to stop", C_MUTED);
}

static void screen_recording(const ui_model_t *m) {
    card();
    char buf[40];
    disp_circle(IN_X + 3, CARD_Y + 10, 3, C_BAD);
    disp_ftext(&font_label, IN_X + 11, CARD_Y + 4, "Recording", C_TEAL);
    unsigned s = (unsigned)m->elapsed_s;
    snprintf(buf, sizeof(buf), "%02u:%02u", s / 60 % 100, s % 60);
    disp_ftext_centered(&font_big, DISP_W / 2, CARD_Y + 13, buf, C_TEXT);
    ICON(IN_X, CARD_Y + 53, CLAW, 1, C_BAD, 0);
    snprintf(buf, sizeof(buf), "%lu scratch episode%s", (unsigned long)m->scratches, plural(m->scratches));
    disp_ftext(&font_body, IN_X + 14, CARD_Y + 50, buf, C_TEXT);
    if (m->scratchy) {  // a red pill while it looks like scratching right now
        disp_round_rect(IN_X, CARD_Y + 66, IN_W, 15, 7, C_BAD);
        disp_ftext_centered(&font_label, DISP_W / 2, CARD_Y + 68, "Scratching detected", C_WHITE);
    } else if (card_b == FULL_B) {
        disp_ftext(&font_label, IN_X, CARD_Y + 68, "Tap for sensors", C_MUTED);
    }
}

static void screen_saved(const ui_model_t *m) {
    card();
    disp_circle(IN_X + 14, CARD_Y + 20, 13, C_TEAL);
    ICON(IN_X + 5, CARD_Y + 14, CHECK, 2, C_WHITE, 0);
    disp_ftext(&font_title, IN_X + 36, CARD_Y + 8, "Saved", C_TEAL);
    char buf[48];
    snprintf(buf, sizeof(buf), "%.0f min, %lu scratch%s", m->last_minutes, (unsigned long)m->last_scratches,
             m->last_scratches == 1 ? "" : "es");  // "episodes" doesn't fit a long recording
    disp_ftext(&font_body, IN_X, CARD_Y + 44, buf, C_TEXT);
    const char *sync = !m->sm->linked ? "Syncs when connected" : m->sm->outbox ? "Sending to your phone..." : "Sent to your phone";
    disp_ftext(&font_label, IN_X, CARD_Y + 62, sync, m->sm->linked && !m->sm->outbox ? C_OK : C_MUTED);
}

static void screen_syncing(const ui_model_t *m) {
    card();
    title("Syncing", C_TEAL);
    ICON(IN_R - 18, CARD_Y + 8, SYNC, 2, C_TEAL, 0);
    char buf[40];
    snprintf(buf, sizeof(buf), "%lu readings left", (unsigned long)m->sm->outbox);
    disp_ftext(&font_body, IN_X, CARD_Y + 36, buf, C_TEXT);
    for (int i = 0; i < 3; i++)
        disp_circle(DISP_W / 2 - 14 + i * 14, CARD_Y + 64, 4, (int)(m->now_ms / 300 % 3) == i ? C_TEAL : C_SOFT);
}

static void screen_sensors(const ui_model_t *m) {
    card();
    disp_ftext(&font_label, IN_X, CARD_Y + 4, "Sensors", C_TEAL);
    char mem[16];
    unsigned pct = m->outbox_cap ? (unsigned)(m->sm->outbox * 100 / m->outbox_cap) : 0;
    snprintf(mem, sizeof(mem), "%u%% used", pct);
    row(CARD_Y + 18, "Movement", "OK", C_OK);
    row(CARD_Y + 31, "Skin sensor", m->gsr_state == SENSOR_OK ? "On skin" : m->gsr_state == SENSOR_IDLE ? "Not on skin" : "None",
        m->gsr_state == SENSOR_OK ? C_OK : m->gsr_state == SENSOR_IDLE ? C_WARN : C_MUTED);
    row(CARD_Y + 44, "Skin temp", m->temp_state == SENSOR_OK ? "OK" : m->temp_state == SENSOR_IDLE ? "Settling" : "None",
        m->temp_state == SENSOR_OK ? C_OK : m->temp_state == SENSOR_IDLE ? C_WARN : C_MUTED);
    row(CARD_Y + 57, "Phone", m->sm->linked ? "Connected" : "Not connected", m->sm->linked ? C_OK : C_MUTED);
    row(CARD_Y + 70, "Memory", mem, pct >= 80 ? C_BAD : C_OK);
    if (card_b == FULL_B) footer("Tap to go back", C_MUTED);
}

static void screen_error(void) {
    card();
    ICON(IN_X, CARD_Y + 7, WARN, 2, C_BAD, C_WHITE);
    disp_ftext(&font_body, IN_X + 24, CARD_Y + 9, "Sensor problem", C_BAD);
    disp_ftext_wrapped(&font_body, IN_X, CARD_Y + 32, IN_W, 0, "The movement sensor isn't answering. Check its wires: SDA 21, SCL 22.", C_TEXT);
    footer("Retrying...", C_MUTED);
}

static void alert_card(const ui_model_t *m) {
    disp_round_rect(CARD_X - 3, CARD_Y - 3, CARD_W + 6, FULL_B - CARD_Y + 6, 11, C_BAD);
    disp_round_rect(CARD_X, CARD_Y, CARD_W, FULL_B - CARD_Y, 8, C_CARD);
    ICON(IN_X, CARD_Y + 5, FLOWER, 2, C_TEAL, C_SOFT);
    char buf[40];
    snprintf(buf, sizeof(buf), "Flare risk %s", m->alert_level[0] ? m->alert_level : "high");
    disp_ftext(&font_label, IN_X + 24, CARD_Y + 4, "Heads up", C_BAD);
    disp_ftext(&font_body, IN_X + 24, CARD_Y + 15, buf, C_TEXT);
    int y = disp_ftext_wrapped(&font_body, IN_X, CARD_Y + 31, IN_W, -2, m->alert_reason, C_TEXT);
    disp_ftext_wrapped(&font_label, IN_X, y + 1, IN_W, -3, m->alert_tip, C_MUTED);
    disp_ftext(&font_label, IN_X, FULL_B - 13, "Double-tap to dismiss", C_TEAL);
}

static void warning_strip(ui_warning_t w) {
    static const char *text[] = {"", "Memory nearly full", "Skin sensor not on skin", "Temp sensor not reading",
                                 "Offline: saving on band"};
    uint16_t bg = w == WARN_STORAGE ? C_BAD : w == WARN_OFFLINE ? C_DEEP : C_WARN;
    disp_round_rect(CARD_X, WARN_Y, CARD_W, 16, 6, bg);
    ICON(CARD_X + 6, WARN_Y + 4, WARN, 1, C_WHITE, bg);
    disp_ftext(&font_label, CARD_X + 18, WARN_Y + 2, text[w], C_WHITE);
}

void ui_render(const ui_model_t *m) {
    static ui_flow_t flow;
    static bool flow_ready;
    if (!flow_ready) ui_flow_init(&flow), flow_ready = true;
    ui_inputs_t in = {.state = m->sm->state, .linked = m->sm->linked, .page = m->sm->page, .held_ms = m->held_ms,
                      .outbox = m->sm->outbox, .outbox_cap = m->outbox_cap, .gsr = m->gsr_state,
                      .temp = m->temp_state, .now_ms = m->now_ms};
    ui_screen_t scr = ui_flow_screen(&flow, &in);
    ui_warning_t warn = ui_flow_warning(&flow, &in);
    if (m->preview_screen) scr = (ui_screen_t)(m->preview_screen - 1);
    if (m->preview_warning) warn = (ui_warning_t)(m->preview_warning - 1);

    disp_fill(C_BG);
    if (scr == SCR_BOOT) {
        screen_boot();
        disp_flush();
        return;
    }
    bool alert = m->sm->alert;
    card_b = warn && !alert ? WARN_Y - 3 : FULL_B;
    header(m);
    switch (scr) {
    case SCR_CONNECT: screen_connect(m); break;
    case SCR_HOME: screen_home(m); break;
    case SCR_HOLD_START: screen_hold(m, false); break;
    case SCR_HOLD_STOP: screen_hold(m, true); break;
    case SCR_STARTED: screen_started(); break;
    case SCR_RECORDING: screen_recording(m); break;
    case SCR_SAVED: screen_saved(m); break;
    case SCR_SYNCING: screen_syncing(m); break;
    case SCR_SENSORS: screen_sensors(m); break;
    case SCR_SENSOR_ERROR: screen_error(); break;
    default: break;
    }
    if (alert) alert_card(m);
    else if (warn) warning_strip(warn);
    disp_flush();
}
