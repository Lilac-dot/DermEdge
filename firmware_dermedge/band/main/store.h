// The outbox in flash: readings waiting for the phone, kept across restarts and flat batteries.
// Plain C over a small flash interface, so the host tests run it on a simulated chip.
//
// A log of 16-byte items in 4 KB sectors: each sector starts with a header (magic, sequence
// number) and holds 255 items. Items are appended at the head; the oldest unsent one is the tail.
// Byte 2 of every item carries two flags, cleared (which NOR flash allows without erasing) as the
// item moves on: INCOMPLETE, cleared once all 16 bytes are written, so an item half-written when
// the power went is skipped; UNSENT, cleared when the phone has it, so a restart picks up exactly
// where it stopped. A sector is erased only when the head wraps onto it,
// once per full cycle (about 20 hours at the band's rate), far inside the flash's endurance.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define STORE_SECTOR 4096
#define STORE_ITEM 16
#define STORE_PER_SECTOR ((STORE_SECTOR - STORE_ITEM) / STORE_ITEM)  // 255 after the header
#define STORE_UNSENT 0x80      // byte 2: set until the item has been sent
#define STORE_INCOMPLETE 0x40  // byte 2: set until all 16 bytes are written
#define STORE_FLAGS (STORE_UNSENT | STORE_INCOMPLETE)

typedef struct {
    int (*read)(void *ctx, uint32_t off, void *buf, uint32_t len);
    int (*write)(void *ctx, uint32_t off, const void *buf, uint32_t len);  // can only clear bits
    int (*erase)(void *ctx, uint32_t off);                                 // one sector, to 0xFF
    void *ctx;
    uint32_t size;  // bytes, a multiple of STORE_SECTOR
} store_flash_t;

typedef struct {
    store_flash_t f;
    uint32_t nsec, seq;
    uint32_t head_sec, head_slot;  // where the next item goes
    uint32_t tail_sec, tail_slot;  // the oldest unsent item
    uint32_t count;                // unsent items
} store_t;

// Scans the flash and picks up where it left off (formats it if there's nothing valid). 0 on success.
int store_open(store_t *s, store_flash_t f);
uint32_t store_capacity(const store_t *s);
uint32_t store_count(const store_t *s);
// Appends a 16-byte item (its byte 2 must leave the top two bits free). False when full: the next
// sector still holds unsent items. With overwrite, that sector's items are dropped instead.
bool store_append(store_t *s, const void *item, bool overwrite);
// The oldest unsent item (flags cleared), false if none
bool store_peek(store_t *s, void *item);
// The n-th oldest unsent item (0 = the next to go out), for the "peek" command; scans, so slow
// for a large n
bool store_peek_nth(store_t *s, uint32_t n, void *item);
// Marks the oldest unsent item sent
void store_pop(store_t *s);

// The band's rule for a filling outbox: older data is kept, new data thins out. Below 90% full
// everything is stored; from 90%, motion summaries only every 10 s; from 98%, scratch episodes
// only (and a scratch episode may overwrite the oldest sector when it's completely full).
#define STORE_THIN 0.90f
#define STORE_SCRATCH_ONLY 0.98f
bool store_should_keep(float fullness, bool is_scratch, bool is_motion, float t);
