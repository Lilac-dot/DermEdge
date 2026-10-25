#include "store.h"

#include <string.h>

#define MAGIC 0x424F4544u  // "DEOB"

typedef struct {
    uint32_t magic, seq, spare, check;  // check = ~seq: a half-written header doesn't count
} header_t;
_Static_assert(sizeof(header_t) == STORE_ITEM, "the header fills one item slot");

static uint32_t item_off(uint32_t sec, uint32_t slot) { return sec * STORE_SECTOR + STORE_ITEM * (1 + slot); }

static bool header(store_t *s, uint32_t sec, uint32_t *seq) {
    header_t h;
    if (s->f.read(s->f.ctx, sec * STORE_SECTOR, &h, sizeof(h))) return false;
    if (h.magic != MAGIC || h.check != ~h.seq) return false;
    *seq = h.seq;
    return true;
}

static int start_sector(store_t *s, uint32_t sec) {
    if (s->f.erase(s->f.ctx, sec * STORE_SECTOR)) return -1;
    header_t h = {MAGIC, ++s->seq, 0xFFFFFFFFu, ~s->seq};
    return s->f.write(s->f.ctx, sec * STORE_SECTOR, &h, sizeof(h));
}

// -1 empty, 1 unsent, 0 sent or half-written. A slot is empty only if all 16 bytes are erased:
// writing over a half-written item would blend the two (flash can only clear bits), so it's
// skipped like a sent one.
static int slot_state(store_t *s, uint32_t sec, uint32_t slot) {
    uint8_t b[STORE_ITEM];
    s->f.read(s->f.ctx, item_off(sec, slot), b, STORE_ITEM);
    bool erased = true;
    for (int i = 0; i < STORE_ITEM; i++) erased &= b[i] == 0xFF;
    if (erased) return -1;
    if (b[2] & STORE_INCOMPLETE) return 0;
    return (b[2] & STORE_UNSENT) ? 1 : 0;
}

// Moves the tail forward to the next unsent item (or the head)
static void seek_tail(store_t *s) {
    while (!(s->tail_sec == s->head_sec && s->tail_slot >= s->head_slot)) {
        if (s->tail_slot >= STORE_PER_SECTOR) {
            s->tail_sec = (s->tail_sec + 1) % s->nsec;
            s->tail_slot = 0;
            continue;
        }
        if (slot_state(s, s->tail_sec, s->tail_slot) == 1) return;
        s->tail_slot++;
    }
}

int store_open(store_t *s, store_flash_t f) {
    memset(s, 0, sizeof(*s));
    s->f = f;
    s->nsec = f.size / STORE_SECTOR;
    if (s->nsec < 2) return -1;
    // the newest sector is the head
    bool any = false;
    for (uint32_t i = 0; i < s->nsec; i++) {
        uint32_t q;
        if (header(s, i, &q) && (!any || q > s->seq)) s->seq = q, s->head_sec = i, any = true;
    }
    if (!any) {  // blank or foreign: start fresh
        s->seq = 0;
        if (start_sector(s, 0)) return -1;
        return 0;
    }
    while (s->head_slot < STORE_PER_SECTOR && slot_state(s, s->head_sec, s->head_slot) != -1) s->head_slot++;
    // the oldest valid sector after the head (going round) is where the log starts
    uint32_t start = s->head_sec, q;
    for (uint32_t k = 1; k < s->nsec; k++) {
        uint32_t i = (s->head_sec + k) % s->nsec;
        if (header(s, i, &q)) {
            start = i;
            break;
        }
    }
    // count what's unsent, from the start to the head
    for (uint32_t sec = start;; sec = (sec + 1) % s->nsec) {
        if (header(s, sec, &q)) {
            uint32_t end = sec == s->head_sec ? s->head_slot : STORE_PER_SECTOR;
            for (uint32_t slot = 0; slot < end; slot++) s->count += slot_state(s, sec, slot) == 1;
        }
        if (sec == s->head_sec) break;
    }
    s->tail_sec = start;
    s->tail_slot = 0;
    seek_tail(s);
    return 0;
}

uint32_t store_capacity(const store_t *s) { return (s->nsec - 1) * STORE_PER_SECTOR; }
uint32_t store_count(const store_t *s) { return s->count; }

bool store_append(store_t *s, const void *item, bool overwrite) {
    if (s->head_slot >= STORE_PER_SECTOR) {
        uint32_t next = (s->head_sec + 1) % s->nsec;
        if (s->count && s->tail_sec == next) {  // the next sector still has unsent items
            if (!overwrite) return false;
            for (uint32_t slot = 0; slot < STORE_PER_SECTOR; slot++) s->count -= slot_state(s, next, slot) == 1;
            s->tail_sec = (next + 1) % s->nsec;  // the sector after it is now the oldest
            s->tail_slot = 0;
        }
        if (start_sector(s, next)) return false;
        s->head_sec = next;
        s->head_slot = 0;
        if (!s->count) s->tail_sec = next, s->tail_slot = 0;
        else seek_tail(s);
    }
    uint8_t b[STORE_ITEM];
    memcpy(b, item, STORE_ITEM);
    b[2] |= STORE_FLAGS;
    uint32_t off = item_off(s->head_sec, s->head_slot);
    s->head_slot++;  // even if the write fails: that slot's bytes may be part-written
    if (s->f.write(s->f.ctx, off, b, STORE_ITEM)) return false;
    b[2] &= (uint8_t)~STORE_INCOMPLETE;  // all 16 bytes are down: mark it whole
    if (s->f.write(s->f.ctx, off + 2, &b[2], 1)) return false;
    if (!s->count) s->tail_sec = s->head_sec, s->tail_slot = s->head_slot - 1;
    s->count++;
    return true;
}

bool store_peek(store_t *s, void *item) {
    if (!s->count) return false;
    uint8_t *b = item;
    s->f.read(s->f.ctx, item_off(s->tail_sec, s->tail_slot), b, STORE_ITEM);
    b[2] &= (uint8_t)~STORE_FLAGS;
    return true;
}

void store_pop(store_t *s) {
    if (!s->count) return;
    uint8_t b[3];
    uint32_t off = item_off(s->tail_sec, s->tail_slot);
    s->f.read(s->f.ctx, off, b, 3);
    b[2] &= (uint8_t)~STORE_UNSENT;
    s->f.write(s->f.ctx, off + 2, &b[2], 1);  // clearing a bit: no erase needed
    s->count--;
    s->tail_slot++;
    seek_tail(s);
}

bool store_peek_nth(store_t *s, uint32_t n, void *item) {
    if (n >= s->count) return false;
    uint32_t sec = s->tail_sec, slot = s->tail_slot;
    for (;;) {
        if (slot >= STORE_PER_SECTOR) {
            sec = (sec + 1) % s->nsec, slot = 0;
            continue;
        }
        if (slot_state(s, sec, slot) == 1 && n-- == 0) {
            uint8_t *b = item;
            s->f.read(s->f.ctx, item_off(sec, slot), b, STORE_ITEM);
            b[2] &= (uint8_t)~STORE_FLAGS;
            return true;
        }
        slot++;
    }
}

bool store_should_keep(float fullness, bool is_scratch, bool is_motion, float t) {
    if (is_scratch) return true;
    if (fullness >= STORE_SCRATCH_ONLY) return false;
    if (fullness >= STORE_THIN && is_motion) return (int)t % 10 == 0;
    return true;
}
