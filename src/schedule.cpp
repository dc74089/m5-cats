#include "schedule.h"

#include <stdio.h>

namespace schedule {

namespace {

// Seconds remaining until `value` reaches the next multiple of `step`.
// Always returns at least 1 so a caller can never derive a zero-length sleep.
int64_t until_next_multiple(int64_t value, int64_t step) {
    const int64_t remainder = value % step;
    return step - remainder;
}

}  // namespace

Rendered describe_elapsed(int64_t elapsed_s) {
    Rendered out{};

    if (elapsed_s < 0) {
        elapsed_s = 0;
    }

    if (elapsed_s < kJustNowUntil) {
        snprintf(out.text, sizeof(out.text), "just now");
        out.next_change_s = static_cast<uint32_t>(kJustNowUntil - elapsed_s);
        return out;
    }

    if (elapsed_s < kHour) {
        const int64_t minutes = (elapsed_s / kMinuteStep) * 15;
        snprintf(out.text, sizeof(out.text), "%lld minutes ago",
                 static_cast<long long>(minutes));
        out.next_change_s =
            static_cast<uint32_t>(until_next_multiple(elapsed_s, kMinuteStep));
        return out;
    }

    if (elapsed_s < kDay) {
        const int64_t hours = elapsed_s / kHour;
        snprintf(out.text, sizeof(out.text), "%lld hour%s ago",
                 static_cast<long long>(hours), hours == 1 ? "" : "s");
        out.next_change_s =
            static_cast<uint32_t>(until_next_multiple(elapsed_s, kHour));
        return out;
    }

    const int64_t days  = elapsed_s / kDay;
    const int64_t hours = (elapsed_s % kDay) / kHour;

    if (hours == 0) {
        snprintf(out.text, sizeof(out.text), "%lld day%s ago",
                 static_cast<long long>(days), days == 1 ? "" : "s");
    } else {
        snprintf(out.text, sizeof(out.text), "%lld day%s %lld hour%s ago",
                 static_cast<long long>(days), days == 1 ? "" : "s",
                 static_cast<long long>(hours), hours == 1 ? "" : "s");
    }
    out.next_change_s =
        static_cast<uint32_t>(until_next_multiple(elapsed_s, kHour));
    return out;
}

uint32_t clamp_sleep(uint32_t seconds) {
    if (seconds < 60) {
        return 60;
    }
    if (seconds > kMaxSleepSeconds) {
        return kMaxSleepSeconds;
    }
    return seconds;
}

namespace {

// Smallest even tick count whose shortest real delay still covers `seconds`.
// One tick on top of ceil() absorbs the short first period; rounding up to
// even avoids the odd-count quirk (see rtc_effective_ticks).
uint32_t even_ticks_for(uint32_t seconds, uint32_t tick) {
    uint32_t n = (seconds + tick - 1) / tick + 1;
    return (n % 2 == 0) ? n : n + 1;
}

// Measured on this unit with `rtctest`: an odd count runs one tick short on
// both sources -- 61 x 1 Hz fires after ~59.8 s, exactly like 60, and 3
// minutes like 2 -- while even counts behave as documented. Thirteen counts
// from 5 to 181, every odd one short, every even one exact.
uint32_t rtc_effective_ticks(RtcTimer t) {
    return t.count >= 2 ? (t.count & ~1u) : t.count;
}

}  // namespace

RtcTimer rtc_timer_for(uint32_t seconds) {
    // Never early, and late by at most three ticks: 3 s, or 3 minutes on
    // the minute source, both inside the display's granularity.
    const uint32_t secs = even_ticks_for(seconds, 1);
    if (secs <= 254) {
        return RtcTimer{static_cast<uint8_t>(secs), false};
    }
    uint32_t minutes = even_ticks_for(seconds, 60);
    if (minutes > 254) {
        minutes = 254;
    }
    return RtcTimer{static_cast<uint8_t>(minutes), true};
}

uint32_t rtc_timer_min_delay(RtcTimer t) {
    const uint32_t tick = t.minutes ? 60 : 1;
    const uint32_t n    = rtc_effective_ticks(t);
    return n == 0 ? 0 : (n - 1) * tick;
}

uint32_t rtc_timer_max_delay(RtcTimer t) {
    const uint32_t tick = t.minutes ? 60 : 1;
    return rtc_effective_ticks(t) * tick;
}

}  // namespace schedule
