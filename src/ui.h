// Screen layout and e-ink refresh strategy.
//
// Only the elapsed-time band changes on a scheduled wake, so that band gets a
// cheap partial refresh and the rest of the screen is left alone. Partial
// refreshes accumulate ghosting, so every kFullRefreshEvery updates the whole
// screen is repainted properly.
#pragma once

#include <stdint.h>
#include <time.h>

namespace ui {

struct State {
    const char *elapsed   = "";  // "5 hours ago"
    const char *subtitle  = "";  // "7:42 am" or "Sat 7:42 am"
    const char *note      = "";  // status line, e.g. the USB-mode banner
    bool        show_undo = false;
    bool        show_fed  = false;  // only in USB interactive mode
    uint16_t    battery_mv = 0;
    bool        on_usb     = false;  // charging: state of charge is meaningless
};

enum class Tap { None, Undo, Fed };

void begin();

// Full panel init-clear. Slow and flashes; used once after a layout change to
// wipe pixels the new layout does not overwrite.
void clear_panel();

// Full-screen repaint (GC16). Slower and flashes, but clears ghosting.
void draw_full(const State &s);

// Repaints only the elapsed-time band (GL16). Used for scheduled wakes.
void draw_elapsed_band(const State &s);

// Waits up to kUndoWindowSeconds for a tap on UNDO, light-sleeping between
// polls. Returns true if UNDO was tapped.
bool wait_for_undo();

// Single non-blocking touch sample, for the USB interactive loop.
Tap poll_tap(const State &s);

// Blocks until the panel has finished refreshing.
//
// The IT8951 displays asynchronously: UpdateArea queues the refresh and
// returns immediately. Cutting power before it finishes leaves the update
// half-drawn, which on battery looks exactly like the device freezing
// mid-redraw. Call this before any wake::power_off(). Returns the wait in ms.
uint32_t wait_for_refresh();

void draw_message(const char *line1, const char *line2);

void draw_diagnostics(uint32_t boots, time_t last_fed, time_t last_sync,
                      uint16_t battery_mv, const char *wake_reason);

}  // namespace ui
