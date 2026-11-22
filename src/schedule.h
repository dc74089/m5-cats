// Pure display-ladder logic: no Arduino, no hardware. Unit-tested in env:native.
//
// Given how long ago the cats were fed, this decides both what the screen should
// say and how long the device may stay powered off before that text goes stale.
// Keeping those two answers in one place is what makes the adaptive wake correct:
// the device sleeps until exactly the moment the string changes, never on a fixed
// tick, which is what holds the wake count to roughly 27 per day.
#pragma once

#include <stdint.h>

namespace schedule {

// Ladder boundaries, in seconds.
constexpr int64_t kJustNowUntil = 15 * 60;    // below this: "just now"
constexpr int64_t kMinuteStep   = 15 * 60;    // 15-minute granularity under an hour
constexpr int64_t kHour         = 60 * 60;
constexpr int64_t kDay          = 24 * kHour;

// The BM8563 timer is an 8-bit counter at 1/60 Hz above 255 s, so a single sleep
// cannot exceed 255 minutes. Four hours leaves headroom.
constexpr uint32_t kMaxSleepSeconds = 4 * 60 * 60;

struct Rendered {
    char     text[48];        // e.g. "5 hours ago"
    uint32_t next_change_s;   // seconds until `text` would differ
};

// elapsed_s < 0 (clock stepped backwards after an NTP sync) is treated as 0.
Rendered describe_elapsed(int64_t elapsed_s);

// Clamp a desired sleep to what the RTC can actually honour. Also enforces a
// floor so a boundary-straddling rounding error cannot cause a wake storm.
uint32_t clamp_sleep(uint32_t seconds);

// A BM8563 countdown: `count` ticks of either a 1 Hz or a 1/60 Hz source.
struct RtcTimer {
    uint8_t count;
    bool    minutes;  // true: 1/60 Hz source, false: 1 Hz
};

// The countdown to program for a sleep of `seconds` (already clamped).
//
// Rounds UP, and only to even counts. M5EPD's setAlarmIRQ() divides by 60
// and truncates, and this BM8563 runs odd counts a tick short, so a typical
// hourly sleep of ~3598 s became 59 minutes that ran like 58: the board woke
// about two minutes early, found the text unchanged, redrew it anyway and
// slept another 60 s -- two boots for every change.
RtcTimer rtc_timer_for(uint32_t seconds);

// Bounds on the real delay a countdown produces, as measured on hardware.
// The first tick lands on the RTC's own next second/minute edge rather than
// a full period after arming, so the shortest delay is one tick less than
// the count; and an odd count behaves like the even count below it.
uint32_t rtc_timer_min_delay(RtcTimer t);
uint32_t rtc_timer_max_delay(RtcTimer t);

}  // namespace schedule
