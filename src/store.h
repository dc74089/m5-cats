// Persistent state across full power-off, in NVS.
//
// Nothing here survives in RAM: the device is genuinely unpowered between
// wakes, so every fact the UI needs must be written to flash before shutdown.
#pragma once

#include <stdint.h>
#include <stddef.h>

#include <time.h>

namespace store {

struct BatterySample {
    uint32_t at;  // epoch seconds, UTC
    uint16_t mv;
} __attribute__((packed));

void begin();
void end();

// 0 means "never fed".
time_t last_fed();
time_t prev_fed();

// Records a feeding. Returns false if it was suppressed as a duplicate
// (another feeding less than kDedupeSeconds earlier).
bool record_feeding(time_t at);

// Reverts the most recent feeding. Returns false if there is nothing to undo.
bool undo_feeding();

// Forgets all feeding history. Test/serial use only.
void clear_feedings();

// Moves feeding times recorded after `boundary` by `delta` seconds.
//
// Only feedings timed against an unvalidated clock share its error. One
// recorded before the last good sync was already correct, and shifting it
// would preserve the error instead of fixing it. Returns how many moved.
size_t shift_feedings_after(time_t boundary, int64_t delta);

// Counts how many partial refreshes have happened since the last full one,
// and reports whether this one should be full.
bool should_full_refresh();
void note_refresh(bool was_full);

// Makes the next render a full one. Used when something outside the
// partial-refresh band (the USB-mode buttons) may still be on the panel.
void force_full_refresh();

time_t last_sync();
void   set_last_sync(time_t at);

// Separate from last_sync: records attempts, successful or not, so a network
// that is simply gone cannot be retried on every wake.
time_t last_sync_attempt();
void   set_last_sync_attempt(time_t at);

uint32_t ui_rev();
void     set_ui_rev(uint32_t rev);

uint32_t boot_count();
uint32_t increment_boot_count();

// Appends to the battery ring, but only if kBatterySampleIntervalSeconds has
// passed since the last sample, to keep NVS writes down.
void maybe_log_battery(time_t now, uint16_t mv);

// Reads the ring oldest-first. Returns how many samples were written to `out`.
size_t read_battery_log(BatterySample *out, size_t max);

}  // namespace store
