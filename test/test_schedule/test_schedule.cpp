#include <unity.h>

#include <stdio.h>
#include <string.h>

#include "schedule.h"

using schedule::describe_elapsed;
using schedule::Rendered;

static void expect(int64_t elapsed, const char *text, uint32_t next) {
    Rendered r = describe_elapsed(elapsed);
    TEST_ASSERT_EQUAL_STRING(text, r.text);
    TEST_ASSERT_EQUAL_UINT32(next, r.next_change_s);
}

void test_just_fed(void) {
    expect(0, "just now", 900);
    expect(1, "just now", 899);
    expect(899, "just now", 1);  // last second before the first boundary
}

void test_quarter_hour_steps(void) {
    expect(900, "15 minutes ago", 900);
    expect(901, "15 minutes ago", 899);
    expect(1800, "30 minutes ago", 900);
    expect(2700, "45 minutes ago", 900);
    expect(3599, "45 minutes ago", 1);
}

void test_whole_hours(void) {
    expect(3600, "1 hour ago", 3600);          // singular
    expect(3601, "1 hour ago", 3599);
    expect(7200, "2 hours ago", 3600);
    expect(5 * 3600 + 42 * 60, "5 hours ago", 18 * 60);
    expect(23 * 3600, "23 hours ago", 3600);
    expect(24 * 3600 - 1, "23 hours ago", 1);
}

void test_days(void) {
    expect(24 * 3600, "1 day ago", 3600);
    expect(28 * 3600, "1 day 4 hours ago", 3600);
    expect(25 * 3600, "1 day 1 hour ago", 3600);  // singular hour inside a day
    expect(48 * 3600, "2 days ago", 3600);
    expect(50 * 3600, "2 days 2 hours ago", 3600);
}

// An NTP sync can step the clock backwards, making "now" earlier than the
// stored feeding time. That must render as "just now", not a negative age.
void test_negative_elapsed_is_clamped(void) {
    expect(-1, "just now", 900);
    expect(-100000, "just now", 900);
}

// next_change_s must never be zero, or the device would schedule a 0-second
// sleep and wake in a tight loop.
void test_next_change_is_never_zero(void) {
    for (int64_t e = 0; e < 50 * 3600; e += 37) {
        Rendered r = describe_elapsed(e);
        TEST_ASSERT_GREATER_THAN_UINT32(0, r.next_change_s);
    }
}

// Sleeping for next_change_s must actually land on a different string,
// otherwise the device burns a wake cycle to redraw identical pixels.
void test_sleeping_until_next_change_actually_changes_text(void) {
    for (int64_t e = 0; e < 50 * 3600; e += 53) {
        Rendered now   = describe_elapsed(e);
        Rendered later = describe_elapsed(e + now.next_change_s);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(
            // one second earlier must still read the same
            describe_elapsed(e + now.next_change_s - 1).text, now.text,
            "text changed before next_change_s elapsed");
        TEST_ASSERT_TRUE_MESSAGE(strcmp(now.text, later.text) != 0,
                                 "text did not change after next_change_s");
    }
}

void test_clamp_sleep(void) {
    TEST_ASSERT_EQUAL_UINT32(60, schedule::clamp_sleep(0));
    TEST_ASSERT_EQUAL_UINT32(60, schedule::clamp_sleep(59));
    TEST_ASSERT_EQUAL_UINT32(900, schedule::clamp_sleep(900));
    TEST_ASSERT_EQUAL_UINT32(schedule::kMaxSleepSeconds,
                             schedule::clamp_sleep(100000));
}

// An early wake finds the text unchanged and costs a second boot to catch the
// real change, so the countdown must never undershoot -- over every sleep the
// firmware can ask for.
void test_rtc_timer_never_wakes_early(void) {
    for (uint32_t s = 60; s <= schedule::kMaxSleepSeconds; s++) {
        const schedule::RtcTimer t = schedule::rtc_timer_for(s);
        if (schedule::rtc_timer_min_delay(t) < s) {
            char msg[64];
            snprintf(msg, sizeof(msg), "sleep %u s can wake early", (unsigned)s);
            TEST_FAIL_MESSAGE(msg);
        }
    }
}

void test_rtc_timer_is_not_needlessly_late(void) {
    for (uint32_t s = 60; s <= schedule::kMaxSleepSeconds; s++) {
        const schedule::RtcTimer t = schedule::rtc_timer_for(s);
        const uint32_t slack = t.minutes ? 180 : 3;
        if (schedule::rtc_timer_max_delay(t) > s + slack) {
            char msg[64];
            snprintf(msg, sizeof(msg), "sleep %u s overshoots", (unsigned)s);
            TEST_FAIL_MESSAGE(msg);
        }
        // Odd counts run a tick short on this BM8563; never program one.
        if (t.count % 2 != 0) {
            char msg[64];
            snprintf(msg, sizeof(msg), "sleep %u s uses odd count %u",
                     (unsigned)s, (unsigned)t.count);
            TEST_FAIL_MESSAGE(msg);
        }
    }
}

// The delay model against the timings measured on the unit with `rtctest`
// (ms elapsed from arming to TF). Each must fall inside the modelled bounds,
// or the never-early guarantee above is checking the wrong thing.
void test_rtc_timer_model_matches_measurements(void) {
    struct Measured {
        uint8_t  count;
        bool     minutes;
        uint32_t ms;
    };
    const Measured kMeasured[] = {
        {5, false, 4013},     {10, false, 9788},    {59, false, 57332},
        {60, false, 59894},   {61, false, 59852},   {62, false, 61805},
        {63, false, 61847},   {91, false, 89861},   {120, false, 119891},
        {121, false, 119849}, {181, false, 179846}, {2, true, 101600},
        {3, true, 118862},
    };
    for (const Measured &m : kMeasured) {
        const schedule::RtcTimer t{m.count, m.minutes};
        // 50 ms of slack for the serial poll loop's own latency.
        const uint32_t lo = schedule::rtc_timer_min_delay(t) * 1000;
        const uint32_t hi = schedule::rtc_timer_max_delay(t) * 1000 + 50;
        if (m.ms <= lo || m.ms > hi) {
            char msg[96];
            snprintf(msg, sizeof(msg), "count %u%s measured %u ms, model %u-%u",
                     (unsigned)m.count, m.minutes ? "m" : "s", (unsigned)m.ms,
                     (unsigned)lo, (unsigned)hi);
            TEST_FAIL_MESSAGE(msg);
        }
    }
}

void test_rtc_timer_source_selection(void) {
    // Short sleeps keep 1-second resolution.
    schedule::RtcTimer t = schedule::rtc_timer_for(60);
    TEST_ASSERT_FALSE(t.minutes);
    TEST_ASSERT_EQUAL_UINT8(62, t.count);
    // The longest sleep must still fit the 8-bit counter.
    t = schedule::rtc_timer_for(schedule::kMaxSleepSeconds);
    TEST_ASSERT_TRUE(t.minutes);
    TEST_ASSERT_EQUAL_UINT8(242, t.count);
    // A typical hourly sleep. The old code made this 59 minutes, which this
    // RTC runs as 58 -- about two minutes early.
    t = schedule::rtc_timer_for(3598);
    TEST_ASSERT_TRUE(t.minutes);
    TEST_ASSERT_EQUAL_UINT8(62, t.count);
    TEST_ASSERT_TRUE(schedule::rtc_timer_min_delay(t) >= 3598);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_just_fed);
    RUN_TEST(test_quarter_hour_steps);
    RUN_TEST(test_whole_hours);
    RUN_TEST(test_days);
    RUN_TEST(test_negative_elapsed_is_clamped);
    RUN_TEST(test_next_change_is_never_zero);
    RUN_TEST(test_sleeping_until_next_change_actually_changes_text);
    RUN_TEST(test_clamp_sleep);
    RUN_TEST(test_rtc_timer_never_wakes_early);
    RUN_TEST(test_rtc_timer_is_not_needlessly_late);
    RUN_TEST(test_rtc_timer_model_matches_measurements);
    RUN_TEST(test_rtc_timer_source_selection);
    return UNITY_END();
}
