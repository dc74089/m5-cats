// Tunables. Everything that might reasonably be changed lives here.
#pragma once

#include <stdint.h>

// --- Feeding semantics -------------------------------------------------
// How long UNDO stays on screen after a feeding is logged.
constexpr uint32_t kUndoWindowSeconds = 60;

// A feeding logged within this long of the previous one is treated as a
// duplicate and ignored. Covers double presses and repeated flashing during
// development, both of which would otherwise look like real feedings.
constexpr uint32_t kDedupeSeconds = 120;

// --- Display -----------------------------------------------------------
// E-ink partial refresh leaves faint ghosting. Force a full repaint this
// often to clear it.
constexpr uint32_t kFullRefreshEvery = 20;

// Orientation. Counter-intuitively, the library's native panel space is
// already landscape (M5EPD_PANEL_W=960, M5EPD_PANEL_H=540), so rotation 0 and
// 2 are landscape 960x540 while 1 and 3 are portrait 540x960. Setting 90 here
// makes UpdateArea clamp a 960-wide canvas to 540 and clip the right half.
// Use 2 if the image is upside down for how the unit hangs.
// The same value feeds the touch controller, whose enum shares these numbers.
constexpr uint8_t kRotation = 0;

constexpr int16_t kScreenW = 960;
constexpr int16_t kScreenH = 540;

// Bump when the layout changes. A mismatch against the stored value triggers
// a one-off panel clear, so pixels from an older layout cannot linger.
constexpr uint32_t kUiRev = 2;

// The band that changes on a scheduled wake, and so is the only part that
// needs repainting. Keeping this tight is what makes partial refresh cheap.
constexpr int16_t kElapsedBandY = 120;
constexpr int16_t kElapsedBandH = 250;

// --- Clock sync --------------------------------------------------------
// WiFi is used only to keep the "(7:42 am)" subtitle honest.
constexpr uint32_t kSyncIntervalSeconds = 7 * 24 * 3600;
// A failed sync must not be retried on every wake. Without this, an
// unreachable network keeps sync_due() true forever and burns ~15 s of WiFi
// on all ~27 wakes a day -- enough to turn months of battery life into weeks.
constexpr uint32_t kSyncRetryIntervalSeconds = 24 * 3600;
constexpr uint32_t kWifiTimeoutMs       = 15000;
constexpr uint32_t kNtpTimeoutMs        = 10000;

// An RTC that has lost power reads year 2000. Anything below this means
// "never set".
constexpr int kMinValidYear = 2020;

// --- Battery log -------------------------------------------------------
// Sampled at most this often, into a fixed ring in NVS. At 6h spacing a
// 120-entry ring holds 30 days, which is enough to fit a discharge slope,
// while keeping NVS writes to ~4/day.
constexpr uint32_t kBatterySampleIntervalSeconds = 6 * 3600;
constexpr size_t   kBatteryRingSize              = 120;

// --- USB / interactive mode ---------------------------------------------
// While powered by something other than our latch, the RTC is kept armed for
// this long and re-armed every second. Unplugging cuts power at once, and the
// board comes back this soon to repaint the screen without the USB buttons.
constexpr uint32_t kUsbExitWakeSeconds = 60;

// Run the CPU slower than the 240 MHz default. Nothing here is compute
// bound, and every boot is dominated by fixed delays in M5.begin().
constexpr uint32_t kCpuMhz = 80;
