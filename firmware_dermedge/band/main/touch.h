// Gestures from one capacitive touch pad (the add-on board's TTP223-style output, high while
// touched). Plain C, fed the pad level every few ms, so the host tests can drive it.
//
//   tap         a short touch, once no second touch follows
//   double tap  two short touches close together
//   long press  held TOUCH_LONG_MS; fires while still held, once. Long on purpose: scratching
//               near the pad must not start or stop a recording.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define TOUCH_DEBOUNCE_MS 30
#define TOUCH_TAP_MAX_MS 400     // longer than this and it's not a tap
#define TOUCH_DOUBLE_GAP_MS 350  // max gap between the two taps of a double tap
#define TOUCH_LONG_MS 2000

typedef enum { TOUCH_NONE, TOUCH_TAP, TOUCH_DOUBLE_TAP, TOUCH_LONG_PRESS } touch_gesture_t;

typedef struct {
    bool raw, stable;
    uint32_t raw_since, pressed_at, released_at;
    bool long_fired, tap_pending;
} touch_t;

void pad_init(touch_t *t);
touch_gesture_t pad_feed(touch_t *t, bool level, uint32_t now_ms);
// How long the pad has been held, for a "keep holding" progress ring; 0 when not held
uint32_t pad_held_ms(const touch_t *t, uint32_t now_ms);
