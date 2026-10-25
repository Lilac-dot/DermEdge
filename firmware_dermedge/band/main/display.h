// 1.8" 128x160 ST7735 in landscape, drawn into a RAM framebuffer and pushed whole, so
// screens change without flicker. Colours are RGB565.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "art.h"
#include "fonts.h"

#define DISP_W 160
#define DISP_H 128

#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

void disp_init(void);   // st7735.c
void disp_flush(void);  // sends the framebuffer to the panel (st7735.c)
const uint16_t *disp_framebuffer(void);  // DISP_H rows of DISP_W big-endian RGB565 pixels

void disp_fill(uint16_t c);
void disp_rect(int x, int y, int w, int h, uint16_t c);
void disp_round_rect(int x, int y, int w, int h, int r, uint16_t c);
void disp_circle(int cx, int cy, int r, uint16_t c);
void disp_pixel(int x, int y, uint16_t c);
// A 1-bit image, rows of `w` pixels packed MSB first, drawn at `scale`
void disp_bitmap(int x, int y, const uint8_t *bits, int w, int h, int scale, uint16_t c);

// 5x7 text at `scale` (a cell is 6*scale wide, 8*scale tall). Returns the x after the text.
int disp_text(int x, int y, const char *s, int scale, uint16_t c);
int disp_text_width(const char *s, int scale);
void disp_text_centered(int cx, int y, const char *s, int scale, uint16_t c);
// Word-wraps into a box `w` wide; returns the y below the last line
int disp_text_wrapped(int x, int y, int w, const char *s, int scale, uint16_t c);

// Anti-aliased text in one of fonts.h's fonts, blended onto what's already drawn. y is the top
// of the line (the baseline sits font->ascent below). Returns the x after the text.
int disp_ftext(const font_t *f, int x, int y, const char *s, uint16_t c);
int disp_ftext_width(const font_t *f, const char *s);
void disp_ftext_centered(const font_t *f, int cx, int y, const char *s, uint16_t c);
// Word-wraps into a box `w` wide, `gap` extra pixels between lines; returns the y below
int disp_ftext_wrapped(const font_t *f, int x, int y, int w, int gap, const char *s, uint16_t c);

// An anti-aliased 4-bit mask (art.h) in colour c, blended like text
void disp_mask(const mask_t *m, int x, int y, uint16_t c);
// A ring around (cx, cy), filled clockwise from 12 o'clock for `frac` (0..1) in fg, rest in bg
void disp_ring(int cx, int cy, int r_out, int r_in, float frac, uint16_t fg, uint16_t bg);
