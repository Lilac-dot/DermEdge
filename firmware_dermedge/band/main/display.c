// Framebuffer and drawing, plain C: st7735.c sends it to the panel, test/preview.c to a PNG.
#include "display.h"

#include <math.h>
#include <string.h>

#include "font5x7.h"

static uint16_t fb[DISP_H][DISP_W];  // big-endian pixels, as the panel wants them

const uint16_t *disp_framebuffer(void) { return &fb[0][0]; }

static inline uint16_t be(uint16_t c) { return (uint16_t)(c >> 8 | c << 8); }

void disp_pixel(int x, int y, uint16_t c) {
    if ((unsigned)x < DISP_W && (unsigned)y < DISP_H) fb[y][x] = be(c);
}

void disp_fill(uint16_t c) {
    disp_rect(0, 0, DISP_W, DISP_H, c);
}

void disp_rect(int x, int y, int w, int h, uint16_t c) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > DISP_W ? DISP_W : x + w, y1 = y + h > DISP_H ? DISP_H : y + h;
    uint16_t v = be(c);
    for (int j = y0; j < y1; j++)
        for (int i = x0; i < x1; i++) fb[j][i] = v;
}

void disp_round_rect(int x, int y, int w, int h, int r, uint16_t c) {
    disp_rect(x + r, y, w - 2 * r, h, c);
    disp_rect(x, y + r, r, h - 2 * r, c);
    disp_rect(x + w - r, y + r, r, h - 2 * r, c);
    for (int j = 0; j < r; j++)  // corners: quarter circles
        for (int i = 0; i < r; i++) {
            int dx = r - i, dy = r - j;
            if (dx * dx + dy * dy > r * r + r) continue;
            disp_pixel(x + i, y + j, c);
            disp_pixel(x + w - 1 - i, y + j, c);
            disp_pixel(x + i, y + h - 1 - j, c);
            disp_pixel(x + w - 1 - i, y + h - 1 - j, c);
        }
}

void disp_circle(int cx, int cy, int r, uint16_t c) {
    for (int j = -r; j <= r; j++)
        for (int i = -r; i <= r; i++)
            if (i * i + j * j <= r * r + r) disp_pixel(cx + i, cy + j, c);
}

void disp_bitmap(int x, int y, const uint8_t *bits, int w, int h, int scale, uint16_t c) {
    int stride = (w + 7) / 8;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            if (bits[j * stride + i / 8] & (0x80 >> (i % 8))) disp_rect(x + i * scale, y + j * scale, scale, scale, c);
}

int disp_text(int x, int y, const char *s, int scale, uint16_t c) {
    for (; *s; s++, x += 6 * scale) {
        unsigned ch = (unsigned char)*s;
        if (ch < 32 || ch > 126) ch = '?';
        const uint8_t *g = font5x7[ch - 32];
        for (int col = 0; col < 5; col++)
            for (int row = 0; row < 7; row++)
                if (g[col] >> row & 1) disp_rect(x + col * scale, y + row * scale, scale, scale, c);
    }
    return x;
}

int disp_text_width(const char *s, int scale) {
    int n = (int)strlen(s);
    return n ? n * 6 * scale - scale : 0;  // no gap after the last glyph
}

void disp_text_centered(int cx, int y, const char *s, int scale, uint16_t c) {
    disp_text(cx - disp_text_width(s, scale) / 2, y, s, scale, c);
}

int disp_text_wrapped(int x, int y, int w, const char *s, int scale, uint16_t c) {
    int per_line = w / (6 * scale);
    char line[64];
    while (*s) {
        while (*s == ' ') s++;
        int len = (int)strlen(s), cut = len;
        if (len > per_line) {
            cut = per_line;
            while (cut > 0 && s[cut] != ' ') cut--;
            if (cut == 0) cut = per_line;  // one long word: break it
        }
        if (cut > (int)sizeof(line) - 1) cut = sizeof(line) - 1;
        memcpy(line, s, cut);
        line[cut] = 0;
        disp_text(x, y, line, scale, c);
        y += 9 * scale;
        s += cut;
    }
    return y;
}

// --- anti-aliased fonts ---------------------------------------------------------------
static const glyph_t *glyph(const font_t *f, char ch) {
    unsigned c = (unsigned char)ch;
    return &f->glyphs[(c < 32 || c > 126 ? '?' : c) - 32];
}

// Mixes colour c over the pixel by alpha/15
static void blend(int x, int y, uint16_t c, int alpha) {
    if ((unsigned)x >= DISP_W || (unsigned)y >= DISP_H || !alpha) return;
    if (alpha == 15) {
        fb[y][x] = be(c);
        return;
    }
    uint16_t d = be(fb[y][x]);
    int r = ((c >> 11) * alpha + (d >> 11) * (15 - alpha)) / 15;
    int g = (((c >> 5) & 63) * alpha + ((d >> 5) & 63) * (15 - alpha)) / 15;
    int b = ((c & 31) * alpha + (d & 31) * (15 - alpha)) / 15;
    fb[y][x] = be((uint16_t)(r << 11 | g << 5 | b));
}

int disp_ftext(const font_t *f, int x, int y, const char *s, uint16_t c) {
    int base = y + f->ascent;
    for (; *s; s++) {
        const glyph_t *g = glyph(f, *s);
        const uint8_t *bits = f->bits + g->offset;
        for (int i = 0; i < g->w * g->h; i++) {
            int a = i % 2 ? bits[i / 2] & 15 : bits[i / 2] >> 4;
            blend(x + g->x + i % g->w, base + g->y + i / g->w, c, a);
        }
        x += g->advance;
    }
    return x;
}

int disp_ftext_width(const font_t *f, const char *s) {
    int w = 0;
    for (; *s; s++) w += glyph(f, *s)->advance;
    return w;
}

void disp_ftext_centered(const font_t *f, int cx, int y, const char *s, uint16_t c) {
    disp_ftext(f, cx - disp_ftext_width(f, s) / 2, y, s, c);
}

int disp_ftext_wrapped(const font_t *f, int x, int y, int w, int gap, const char *s, uint16_t c) {
    char line[96];
    int line_h = f->ascent + f->descent + gap;
    while (*s) {
        while (*s == ' ') s++;
        if (!*s) break;
        // take words while they fit
        int len = 0, fit = 0;
        while (s[len]) {
            int end = len;
            while (s[end] && s[end] != ' ') end++;
            if (end >= (int)sizeof(line)) break;
            memcpy(line, s, end);
            line[end] = 0;
            if (disp_ftext_width(f, line) > w && fit) break;
            fit = end;
            len = end;
            if (s[len] == ' ') len++;
        }
        if (!fit) fit = len ? len : (int)strlen(s);  // a single word wider than the box
        if (fit > (int)sizeof(line) - 1) fit = sizeof(line) - 1;
        memcpy(line, s, fit);
        line[fit] = 0;
        disp_ftext(f, x, y, line, c);
        y += line_h;
        s += fit;
    }
    return y;
}

void disp_mask(const mask_t *m, int x, int y, uint16_t c) {
    for (int i = 0; i < m->w * m->h; i++) {
        int a = i % 2 ? m->bits[i / 2] & 15 : m->bits[i / 2] >> 4;
        blend(x + i % m->w, y + i / m->w, c, a);
    }
}

void disp_ring(int cx, int cy, int r_out, int r_in, float frac, uint16_t fg, uint16_t bg) {
    for (int j = -r_out; j <= r_out; j++)
        for (int i = -r_out; i <= r_out; i++) {
            int d2 = i * i + j * j;
            if (d2 > r_out * r_out + r_out || d2 < r_in * r_in - r_in) continue;
            float a = atan2f((float)i, (float)-j);  // 0 at 12 o'clock, clockwise
            if (a < 0) a += 2 * 3.14159265f;
            disp_pixel(cx + i, cy + j, a <= frac * 2 * 3.14159265f ? fg : bg);
        }
}
