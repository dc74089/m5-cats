# Cats last fed

A wall-mounted M5Paper showing how long ago the cats were fed.

Press the side button: the device powers on, logs a feeding, shows **UNDO** for
60 seconds, and powers itself back off. E-ink keeps the image on screen the
whole time it is off.

The touchscreen is only live during that 60-second window -- the rest of the
time the board is unpowered, so there is nothing to tap. The UNDO button
disappearing is therefore the signal that the window has closed; the same code
path draws it away and stops polling, so the two cannot disagree. Pressing the
side button again reopens the window even if the press is too soon to count as
a separate feeding.

## How it stays alive for months

The device is **fully powered off** between updates -- not deep sleeping.
`wake::power_off()` arms the RTC and de-latches GPIO2 so only the BM8563 keeps
running, drawing microamps. ESP32 deep sleep is not an option on this board: too many ICs must
stay powered, measuring ~2.6 mA, which would flatten the 1150 mAh cell in about
two weeks.

It wakes only at the exact moment the displayed text would go stale, which is
why the wake count is ~27/day rather than one per minute:

| Elapsed | Shown |
|---|---|
| < 15 min | "just now" |
| 15-59 min | 15-minute steps |
| 1-23 h | whole hours |
| >= 24 h | days and hours |

Estimated budget is ~3.6 mAh/day against ~900 mAh usable, so roughly 8 months.
That is an estimate: the firmware samples battery voltage into a ring buffer in
NVS so the real discharge slope can be read back (`dump`).

## Setup

```sh
python3 -m venv .venv && .venv/bin/pip install platformio pyserial
cp include/secrets.h.example include/secrets.h   # then edit it
.venv/bin/pio run -e m5paper -t upload
```

`include/secrets.h` is gitignored. WiFi is used **only** for a weekly NTP sync
of the clock; elapsed time is a difference between two reads of the same RTC,
so a wrong password degrades the "(7:42 am)" subtitle and nothing else.

## Testing

Unit tests for the display ladder run off-device:

```sh
.venv/bin/pio test -e native
```

With USB connected the board **physically cannot power off**, so it drops into
an interactive mode with an on-screen FED button and a serial console. That
mode never takes the power latch back: unplug and the board is off within
milliseconds, then wakes 60 s later to repaint the normal screen.

```sh
.venv/bin/python tools/serial_probe.py 10 state
```

Commands: `help`, `state`, `dump`, `sync`, `skew <secs>`, `bat`, `undotest`,
`rtctest <secs|Nm|Ns>`, `fed <secs-ago>`, `undo`, `reset`. `fed` backdates the last feeding, which is how the display
ladder is walked without waiting a day; `skew` mis-sets the RTC so the next
`sync` has to correct it. `sync` bypasses the retry backoff, which is what you
want right after editing credentials. `undotest` opens the light-sleep undo
window on demand, which is the only way to watch that path over serial -- on
battery it runs blind. `rtctest` times the RTC countdown the same way:
`rtctest 3598` uses the production rounding, `rtctest 3m` / `61s` a raw count.
`state` ends with a `power:` line listing everything that can hold the board
on except USB -- in interactive mode `gpio2_latch` must be 0 and `TF` 0.

Hold the **left** side button while powering on for a diagnostics screen.

The sleep path can only be verified on battery. See the plan's verification
section.

## Layout

| File | Role |
|---|---|
| `src/main.cpp` | power latch, wake dispatch, one lifecycle per boot |
| `src/wake.cpp` | why did we power on (read before any M5 init); power latch and RTC wake |
| `src/schedule.cpp` | display ladder; pure, unit-tested |
| `src/store.cpp` | NVS: feeding times, undo, battery ring |
| `src/clock.cpp` | RTC in UTC, POSIX TZ for display, NTP |
| `src/ui.cpp` | layout, partial vs full refresh, undo window |
| `boards/m5paper.json` | vendored board definition (see below) |

## Battery indicator

Top right. There is no fuel gauge on this board, only an ADC across a divider,
so the percentage is inferred from a discharge curve in `src/battery.cpp`.

Two things make that workable: the reading is taken at the same point in every
boot, before any EPD refresh, so the load is consistent; and measured noise is
only about +/-4 mV, which held eight consecutive samples at the same value.

It is still an approximation, and deliberately rounded to the nearest 5%. A
LiPo spends most of its life on a flat part of the curve -- 3.83 V to 3.94 V
covers 50% to 70% -- so the middle of the range is inherently coarse. The ends
are much more informative than the middle. While USB is connected the charger
holds the pack up and terminal voltage says nothing about state of charge, so
the header shows `USB` instead of a number.

The header only repaints on a full refresh, roughly every 20 updates, so the
figure does not flicker.

Exact millivolts are on the diagnostics screen (hold the left button at boot),
in the boot log, and from `bat`. Once the ring buffer has a few weeks of
samples, `dump` gives the real discharge curve for this specific cell, which
beats any generic table -- recalibrate `kCurve` from it if you care.

## Orientation

`kRotation` in `include/config.h` sets it. The library's native panel space is
**already landscape** (`M5EPD_PANEL_W` is 960, `M5EPD_PANEL_H` is 540), so 0
and 2 are landscape 960x540 while 1 and 3 are portrait. Setting 90 makes
`UpdateArea` clamp a 960-wide canvas to 540 and silently clip the right half.
Use 2 if the image is upside down for how the unit hangs.

Changing the layout? Bump `kUiRev` too, so the panel gets a one-off clear and
no pixels from the old layout survive.

## Traps worth knowing

**Never take the power latch back.** Power stays on while *any* of four
things holds it: GPIO2 high, the side button, an asserted RTC INT, or USB. So
surviving `power_off()` means "something else is holding power", not "USB is
in" -- and if the firmware then raises GPIO2 again (an earlier version did,
to keep the battery ADC reading), pulling the cable leaves the board running
flat out on battery. That version lasted about two days instead of months.
Interactive mode now keeps GPIO2 low, takes it only for the length of a
`bat` reading, and re-arms the RTC every second, which also clears TF -- in
level mode a set TF keeps INT asserted, and the old code left it set, so the
RTC was holding power too.

**The RTC countdown is neither exact nor documented-correct.** Measured on
this unit with `rtctest`: the first tick lands on the RTC's own next
second/minute edge, so a count of N gives between N-1 and N ticks; and an
*odd* count runs one tick short (61 x 1 Hz fires at ~59.8 s, like 60; 3
minutes like 2). M5EPD's `setAlarmIRQ()` also truncates to whole minutes. So
a typical hourly sleep came out about two minutes early, found nothing to
change, redrew anyway and slept 60 s more: two boots per change.
`schedule::rtc_timer_for()` rounds up to an even count and is unit-tested
against both the measurements and every sleep length the firmware can ask
for. If the board is ever swapped, re-run the `rtctest` points.

**A flat battery stops the RTC.** The BM8563 runs off the main cell, and
after it dies the date registers can hold anything plausible -- one unit came
back reading 2053 and stamped a feeding with it. `rtcclock::is_set()` checks
the VL bit, which the library's `getTime()` masks away.

**The RTC clears its own wake flags.** `BM8563::begin()` -- which
`M5.RTC.begin()` calls -- writes `0x00` to register `0x01`, destroying the TF/AF
flags that say the RTC woke us. `wake::read_boot_flags()` therefore reads them
over raw `Wire` *before* any M5 initialisation. Get this wrong and the device
logs a phantom feeding every hour.

**The panel refreshes asynchronously.** `UpdateArea` queues the refresh on the
IT8951 and returns immediately; the panel then takes ~500 ms to actually draw.
Calling `wake::power_off()` straight afterwards cuts power mid-refresh and leaves
the update half-drawn -- which on battery looks exactly like the device
freezing. `ui::wait_for_refresh()` polls `LUTAFSR` until the display engine
goes idle, and must precede every `wake::power_off()`. This is invisible over USB,
where power is never actually cut.

**A clock correction has to move the feeding times with it.** Only the ones
recorded since the last validated sync, though -- a feeding timed against a
good clock is already right, and shifting it would bake in the error rather
than fix it. This matters once, on the first sync after a fresh unit seeds its
RTC from the build time. Both directions are covered by the scenario tests
described above.

**The board definition is vendored.** `m5stack-paper` ships only on
platform-espressif32's `develop` branch, and its `variant: m5stack_paper` exists
only in Arduino core 3.x -- which the IDF-4-era M5EPD library will not compile
against (`esp_adc_cal.h` was removed in IDF 5). So the platform is pinned to
espressif32 6.x and `boards/m5paper.json` carries the board definition with the
generic `esp32` variant. Pin numbers come from `M5EPD.h`, not the variant.
