#include "touch.h"

#include <string.h>

void pad_init(touch_t *t) {
    memset(t, 0, sizeof(*t));
}

touch_gesture_t pad_feed(touch_t *t, bool level, uint32_t now_ms) {
    if (level != t->raw) {
        t->raw = level;
        t->raw_since = now_ms;
    }
    bool changed = t->raw != t->stable && now_ms - t->raw_since >= TOUCH_DEBOUNCE_MS;
    if (changed) {
        t->stable = t->raw;
        if (t->stable) {  // pressed
            t->pressed_at = now_ms;
            t->long_fired = false;
        } else {  // released
            uint32_t held = now_ms - t->pressed_at;
            t->released_at = now_ms;
            if (!t->long_fired && held <= TOUCH_TAP_MAX_MS) {
                if (t->tap_pending) {
                    t->tap_pending = false;
                    return TOUCH_DOUBLE_TAP;
                }
                t->tap_pending = true;
            } else {
                t->tap_pending = false;  // a long or medium hold cancels a waiting tap
            }
        }
        return TOUCH_NONE;
    }
    if (t->stable && !t->long_fired && now_ms - t->pressed_at >= TOUCH_LONG_MS) {
        t->long_fired = true;
        t->tap_pending = false;
        return TOUCH_LONG_PRESS;
    }
    if (t->tap_pending && !t->stable && now_ms - t->released_at > TOUCH_DOUBLE_GAP_MS) {
        t->tap_pending = false;
        return TOUCH_TAP;
    }
    return TOUCH_NONE;
}

uint32_t pad_held_ms(const touch_t *t, uint32_t now_ms) {
    return t->stable && !t->long_fired ? now_ms - t->pressed_at : 0;
}
