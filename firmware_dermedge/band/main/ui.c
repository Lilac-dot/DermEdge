#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "display.h"
#include "fonts.h"
#include "touch.h"

// The website's palette (web/index.html, light theme), pushed much deeper for the ST7735: its pale
// pastels (petal #F9DDD8, card #FFFAF9, sage #A7B59E) wash out to white on this panel.
#define C_BG RGB(0xD8, 0x4A, 0x78)     // rose pink
#define C_CARD RGB(0xF6, 0xB3, 0xC6)   // pink card
#define C_SAGE RGB(0x5E, 0x8F, 0x48)   // sage
#define C_DEEP RGB(0x1F, 0x55, 0x1A)   // deep green, for titles and icons
#define C_BERRY RGB(0xB0, 0x1E, 0x55)  // berry
#define C_TRACK RGB(0xFC, 0xDE, 0xE7)  // light blush: empty bars, the alert card
#define C_TEXT RGB(0x1E, 0x12, 0x16)   // near black
#define C_MUTED RGB(0x5A, 0x2A, 0x38)  // plum
#define C_BAD RGB(0xA8, 0x10, 0x30)    // deep red
#define C_OK RGB(0x16, 0x6E, 0x40)     // green
#define C_WHITE RGB(0xFF, 0xFF, 0xFF)

// Pixel art: '#' first colour, '+' second, anything else transparent
static const char *LEAF[] = {
    "........###.", "......####+.", "....#####+#.", "...####+###.", "..####+###..", "..###+####..",
    ".###+####...", ".##+####....", ".#+###......", "..+.........", ".+..........", "+...........",
};
static const char *BLUETOOTH[] = {
    "...#...", "...##..", "#..#.#.", ".#.#..#", "..##.#.", "...##..",
    "..##.#.", ".#.#..#", "#..#.#.", "...##..", "...#...",
};
static const char *FLOWER[] = {
    "..++.++..", ".+++++++.", "+++###+++", ".++###++.", "+++###+++", ".+++++++.", "..++.++..", "....#....", "...##....",
};
static const char *SPROUT[] = {
    "##.....##", "###...###", ".###.###.", "..##+##..", "....+....", "....+....", "..+++++..", ".+++++++.",
};
static const char *SYNC[] = {
    "..#......", ".##......", "#########", ".##......", "..#......",
    "......#..", "......##.", "#########", "......##.", "......#..",
};
static const char *WARN[] = {
    "....#....", "...#+#...", "...#+#...", "..##+##..", "..##+##..", ".#######.", ".###+###.", "#########",
};
static const char *PULSE[] = {
    "....#......", "...#.#.....", "...#.#.....", "##.#...#.##", "..#....#.#.", "..#.....#..", "........#..",
};
static const char *CLAW[] = {"#..#..#..", "#..#..#..", ".#..#..#.", ".#..#..#.", "..#..#..#", "..#..#..#"};
static const char *BLOSSOM[] = {".+.", "+#+", ".+."};  // tiny white flower for the header

#define ROWS(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define ICON(x, y, art, scale, c1, c2) icon(x, y, art, ROWS(art), scale, c1, c2)

static void icon(int x, int y, const char **rows, int n, int scale, uint16_t c1, uint16_t c2) {
    for (int j = 0; j < n; j++)
        for (int i = 0; rows[j][i]; i++) {
            char p = rows[j][i];
            if (p == '#' || p == '+') disp_rect(x + i * scale, y + j * scale, scale, scale, p == '#' ? c1 : c2);
        }
}

#define CARD_X 6
#define CARD_Y 22
#define CARD_W 148
#define CARD_H 100
#define IN_X (CARD_X + 8)
#define IN_W (CARD_W - 16)
#define IN_R (CARD_X + CARD_W - 8)  // right edge of the content
#define CARD_B (CARD_Y + CARD_H)

static void header(const ui_model_t *m) {
    ICON(5, 4, LEAF, 1, C_DEEP, C_WHITE);
    int x = disp_ftext(&font_label, 20, 3, "DermEdge", C_WHITE);
    for (int i = 0; i < 3; i++) ICON(x + 6 + i * 9, 7 + (i % 2) * 3, BLOSSOM, 1, C_DEEP, C_WHITE);
    ICON(148, 4, BLUETOOTH, 1, m->sm->linked ? C_WHITE : C_CARD, 0);
    if (m->skin_sim) {  // never let simulated skin readings pass for real ones
        disp_round_rect(84, 3, 26, 13, 4, C_WHITE);
        disp_ftext(&font_label, 87, 3, "SIM", C_BAD);
    }
    if (m->sm->state == ST_RECORDING && (m->now_ms / 500) % 2 == 0) {  // blinking REC
        disp_circle(117, 10, 3, C_WHITE);
        disp_ftext(&font_label, 123, 3, "REC", C_WHITE);
    }
}

// The card with a title in Young Serif and a pixel icon on the right
static void card(const char *title, uint16_t title_c, const char **art, int rows, uint16_t c1, uint16_t c2) {
    disp_round_rect(CARD_X, CARD_Y, CARD_W, CARD_H, 8, C_CARD);
    if (title) disp_ftext(&font_title, IN_X, CARD_Y + 3, title, title_c);
    if (art) icon(IN_R - (int)strlen(art[0]) * 2, CARD_Y + 8, art, rows, 2, c1, c2);
}

// "Hold 2 s" bar along the bottom of the card while the pad is held
static void hold_bar(const ui_model_t *m) {
    if (!m->held_ms) return;
    int w = IN_W * (int)(m->held_ms > TOUCH_LONG_MS ? TOUCH_LONG_MS : m->held_ms) / TOUCH_LONG_MS;
    disp_round_rect(IN_X, CARD_B - 9, IN_W, 5, 2, C_TRACK);
    if (w > 4) disp_round_rect(IN_X, CARD_B - 9, w, 5, 2, C_DEEP);
}

static void claw_line(int y, uint32_t n, uint16_t c) {
    char buf[32];
    ICON(IN_X, y + 3, CLAW, 1, C_BAD, 0);
    snprintf(buf, sizeof(buf), "%lu scratch episode%s", (unsigned long)n, n == 1 ? "" : "s");
    disp_ftext(&font_body, IN_X + 14, y, buf, c);
}

// "label ........ value" on one line
static void row(int y, const char *label, const char *value, uint16_t vc) {
    disp_ftext(&font_body, IN_X, y, label, C_MUTED);
    disp_ftext(&font_body, IN_R - disp_ftext_width(&font_body, value), y, value, vc);
}

static void screen_boot(void) {
    ICON(56, 18, LEAF, 4, C_DEEP, C_WHITE);
    disp_ftext_centered(&font_title, DISP_W / 2, 72, "DermEdge", C_WHITE);
    disp_ftext_centered(&font_label, DISP_W / 2, 102, "starting...", C_CARD);
}

static void screen_pairing(const ui_model_t *m) {
    card("Connect", C_DEEP, BLUETOOTH, ROWS(BLUETOOTH), C_DEEP, 0);
    char buf[40];
    if (m->sm->passkey) {
        snprintf(buf, sizeof(buf), "%03lu %03lu", (unsigned long)m->sm->passkey / 1000,
                 (unsigned long)m->sm->passkey % 1000);
        disp_ftext(&font_label, IN_X, CARD_Y + 31, "Pairing code", C_MUTED);
        disp_ftext_centered(&font_big, DISP_W / 2, CARD_Y + 38, buf, C_TEXT);
        disp_ftext_centered(&font_body, DISP_W / 2, CARD_Y + 76, "Type it on your laptop", C_MUTED);
    } else {
        int y = disp_ftext_wrapped(&font_body, IN_X, CARD_Y + 32, IN_W, 0,
                                   "Open DermEdge in Chrome and tap Connect band", C_TEXT);
        if (m->sm->outbox) {
            snprintf(buf, sizeof(buf), "%lu readings to sync", (unsigned long)m->sm->outbox);
            disp_ftext(&font_label, IN_X, y + 2, buf, C_DEEP);
        }
        disp_ftext(&font_label, IN_X, CARD_B - 24, "Hold 2 s to record offline", C_MUTED);
    }
    hold_bar(m);
}

static void screen_ready(const ui_model_t *m) {
    card("Ready", C_DEEP, SPROUT, ROWS(SPROUT), C_SAGE, C_WHITE);
    disp_ftext_wrapped(&font_body, IN_X, CARD_Y + 32, IN_W, 0, "Hold the pad 2 s to start recording", C_TEXT);
    if (m->has_last) {
        char buf[40];
        snprintf(buf, sizeof(buf), "Last session, %.0f min", m->last_minutes);
        disp_ftext(&font_label, IN_X, CARD_Y + 64, buf, C_MUTED);
        claw_line(CARD_Y + 76, m->last_scratches, C_TEXT);
    }
    hold_bar(m);
}

static void screen_recording(const ui_model_t *m) {
    disp_round_rect(CARD_X, CARD_Y, CARD_W, CARD_H, 8, C_CARD);
    static const char *dots[] = {"", ".", "..", "..."};
    char buf[40];
    snprintf(buf, sizeof(buf), "Collecting data%s", dots[(m->now_ms / 400) % 4]);
    disp_ftext(&font_label, IN_X, CARD_Y + 5, buf, C_DEEP);
    ICON(IN_R - 22, CARD_Y + 6, PULSE, 2, m->scratchy ? C_BAD : C_SAGE, 0);
    if (m->sm->page == 0) {
        unsigned s = (unsigned)m->elapsed_s;
        snprintf(buf, sizeof(buf), "%02u:%02u", s / 60 % 100, s % 60);
        disp_ftext_centered(&font_big, DISP_W / 2, CARD_Y + 14, buf, C_TEXT);
        // movement bar: sage when calm, red while it looks like scratching
        disp_ftext(&font_label, IN_X, CARD_Y + 49, m->scratchy ? "Scratching?" : "Movement", m->scratchy ? C_BAD : C_MUTED);
        disp_round_rect(IN_X, CARD_Y + 62, IN_W, 8, 3, C_TRACK);
        float level = m->activity / 0.6f;
        int w = (int)(IN_W * (level > 1 ? 1 : level));
        if (w > 6) disp_round_rect(IN_X, CARD_Y + 62, w, 8, 3, m->scratchy ? C_BAD : C_SAGE);
        claw_line(CARD_Y + 76, m->scratches, C_TEXT);
    } else {  // detail page: six rows, 12 px apart
        snprintf(buf, sizeof(buf), "%.1f Hz", m->freq_hz);
        row(CARD_Y + 18, "Stroke rate", buf, C_TEXT);
        snprintf(buf, sizeof(buf), "%.3f g", m->activity);
        row(CARD_Y + 30, "Movement", buf, C_TEXT);
        if (m->gsr_ok) snprintf(buf, sizeof(buf), "%.2f uS", m->gsr_us);
        row(CARD_Y + 42, "GSR", m->gsr_ok ? buf : "not on skin", m->gsr_ok ? C_TEXT : C_MUTED);
        if (m->temp_ok) snprintf(buf, sizeof(buf), "%.1f C", m->skin_c);
        row(CARD_Y + 54, "Skin temp", m->temp_ok ? buf : "no sensor", m->temp_ok ? C_TEXT : C_MUTED);
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)m->sm->outbox);
        row(CARD_Y + 66, "Waiting to send", buf, C_TEXT);
        row(CARD_Y + 78, "Laptop", m->sm->linked ? "connected" : "offline", m->sm->linked ? C_OK : C_MUTED);
    }
    hold_bar(m);
}

static void screen_syncing(const ui_model_t *m) {
    card("Syncing", C_DEEP, SYNC, ROWS(SYNC), C_DEEP, 0);
    char buf[40];
    snprintf(buf, sizeof(buf), "%lu readings left", (unsigned long)m->sm->outbox);
    disp_ftext(&font_body, IN_X, CARD_Y + 36, buf, C_TEXT);
    for (int i = 0; i < 3; i++)
        disp_circle(DISP_W / 2 - 14 + i * 14, CARD_Y + 68, 4, (int)(m->now_ms / 300 % 3) == i ? C_DEEP : C_TRACK);
    hold_bar(m);
}

static void screen_error(void) {
    card("Sensor?", C_BAD, WARN, ROWS(WARN), C_BAD, C_WHITE);
    disp_ftext_wrapped(&font_body, IN_X, CARD_Y + 32, IN_W, 0,
                       "Accelerometer not found. Check its wires: SDA 21, SCL 22.", C_TEXT);
    disp_ftext(&font_label, IN_X, CARD_B - 16, "Retrying...", C_MUTED);
}

static void alert_card(const ui_model_t *m) {
    disp_round_rect(CARD_X - 3, CARD_Y - 3, CARD_W + 6, CARD_H + 6, 11, C_BERRY);
    disp_round_rect(CARD_X, CARD_Y, CARD_W, CARD_H, 8, C_TRACK);
    ICON(IN_X, CARD_Y + 5, FLOWER, 2, C_DEEP, C_BERRY);
    char buf[40];
    snprintf(buf, sizeof(buf), "Flare risk %s", m->alert_level[0] ? m->alert_level : "high");
    disp_ftext(&font_label, IN_X + 24, CARD_Y + 4, "Heads up", C_BAD);
    disp_ftext(&font_body, IN_X + 24, CARD_Y + 15, buf, C_TEXT);
    // tight line spacing: two lines of reason and two of tip fit above the footer
    int y = disp_ftext_wrapped(&font_body, IN_X, CARD_Y + 31, IN_W, -2, m->alert_reason, C_TEXT);
    disp_ftext_wrapped(&font_label, IN_X, y + 1, IN_W, -3, m->alert_tip, C_MUTED);
    disp_ftext(&font_label, IN_X, CARD_B - 13, "Double-tap to dismiss", C_DEEP);
}

void ui_render(const ui_model_t *m) {
    disp_fill(C_BG);
    if (m->sm->state == ST_BOOT) {
        screen_boot();
    } else {
        header(m);
        switch (m->sm->state) {
        case ST_PAIRING: screen_pairing(m); break;
        case ST_READY: screen_ready(m); break;
        case ST_RECORDING: screen_recording(m); break;
        case ST_SYNCING: screen_syncing(m); break;
        case ST_ERROR: screen_error(); break;
        default: break;
        }
        if (m->sm->alert) alert_card(m);
    }
    disp_flush();
}
