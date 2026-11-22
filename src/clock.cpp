#include "clock.h"

#include <M5EPD.h>
#include <WiFi.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "secrets.h"

namespace rtcclock {
namespace {

// Howard Hinnant's civil-date algorithm. Used instead of timegm() so the
// conversion does not depend on which newlib variant the core ships.
int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int      era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
}

// BM8563 seconds register. Its top bit, VL, is set whenever the oscillator
// has stopped -- i.e. the battery went flat -- and stays set until the time
// is rewritten, which set_utc() does.
constexpr uint8_t kRegSeconds    = 0x02;
constexpr uint8_t kVoltageLowBit = 0x80;

bool g_tz_applied = false;

void apply_timezone() {
    if (g_tz_applied) {
        return;
    }
    setenv("TZ", TIMEZONE, 1);
    tzset();
    g_tz_applied = true;
}

}  // namespace

void begin() {
    M5.RTC.begin();  // note: this clears the wake flags, so wake::read_boot_flags
                     // must already have run
    apply_timezone();
}

bool clock_integrity_lost() {
    return (M5.RTC.readReg(kRegSeconds) & kVoltageLowBit) != 0;
}

bool is_set() {
    // The year check alone is not enough: after the cell runs flat the
    // registers can hold any plausible-looking date, and the library's
    // getTime() masks VL away. That is how a feeding got stamped 2053.
    if (clock_integrity_lost()) {
        return false;
    }
    rtc_date_t date;
    M5.RTC.getDate(&date);
    return date.year >= kMinValidYear;
}

time_t now_utc() {
    rtc_date_t date;
    rtc_time_t t;
    M5.RTC.getDate(&date);
    M5.RTC.getTime(&t);

    const int64_t days = days_from_civil(date.year, static_cast<unsigned>(date.mon),
                                         static_cast<unsigned>(date.day));
    return static_cast<time_t>(days * 86400LL + t.hour * 3600LL + t.min * 60LL +
                               t.sec);
}

time_t epoch_from_utc(int year, int mon, int day, int hour, int min, int sec) {
    const int64_t days = days_from_civil(year, static_cast<unsigned>(mon),
                                         static_cast<unsigned>(day));
    return static_cast<time_t>(days * 86400LL + hour * 3600LL + min * 60LL + sec);
}

void set_utc(time_t t) {
    struct tm utc;
    gmtime_r(&t, &utc);

    rtc_date_t date;
    date.year = static_cast<int16_t>(utc.tm_year + 1900);
    date.mon  = static_cast<int8_t>(utc.tm_mon + 1);
    date.day  = static_cast<int8_t>(utc.tm_mday);
    date.week = static_cast<int8_t>(utc.tm_wday);

    rtc_time_t time;
    time.hour = static_cast<int8_t>(utc.tm_hour);
    time.min  = static_cast<int8_t>(utc.tm_min);
    time.sec  = static_cast<int8_t>(utc.tm_sec);

    M5.RTC.setDate(&date);
    M5.RTC.setTime(&time);
}

void format_local_time(time_t t, char *buf, size_t n) {
    apply_timezone();
    struct tm local;
    localtime_r(&t, &local);
    int hour12 = local.tm_hour % 12;
    if (hour12 == 0) {
        hour12 = 12;
    }
    snprintf(buf, n, "%d:%02d %s", hour12, local.tm_min,
             local.tm_hour < 12 ? "am" : "pm");
}

void format_local_day_time(time_t t, char *buf, size_t n) {
    apply_timezone();
    struct tm local;
    localtime_r(&t, &local);
    static const char *kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    int hour12 = local.tm_hour % 12;
    if (hour12 == 0) {
        hour12 = 12;
    }
    snprintf(buf, n, "%s %d:%02d %s", kDays[local.tm_wday % 7], hour12, local.tm_min,
             local.tm_hour < 12 ? "am" : "pm");
}

bool sync_due(time_t last_sync, time_t last_attempt) {
    const time_t now = now_utc();

    // Rate-limit attempts first. This is what stops an unreachable network
    // from costing a WiFi connect on every single wake.
    //
    // A stored attempt time in the future means the clock stepped backwards
    // since it was written. Treat that as "just attempted" rather than
    // letting the comparison fall through, which would disable the rate
    // limit precisely when it is most needed.
    if (last_attempt != 0) {
        const uint32_t since_attempt =
            now >= last_attempt ? static_cast<uint32_t>(now - last_attempt) : 0;
        if (since_attempt < kSyncRetryIntervalSeconds) {
            return false;
        }
    }
    if (!is_set() || last_sync == 0) {
        return true;
    }
    return now < last_sync ||
           static_cast<uint32_t>(now - last_sync) >= kSyncIntervalSeconds;
}

bool sync_ntp() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    const uint32_t started = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - started > kWifiTimeoutMs) {
            WiFi.disconnect(true, true);
            WiFi.mode(WIFI_OFF);
            return false;
        }
        delay(100);
    }

    // Ask for UTC; local presentation is handled by our own TZ string.
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");

    bool ok = false;
    const uint32_t ntp_started = millis();
    while (millis() - ntp_started < kNtpTimeoutMs) {
        const time_t now = time(nullptr);
        struct tm utc;
        gmtime_r(&now, &utc);
        if (utc.tm_year + 1900 >= kMinValidYear) {
            set_utc(now);
            ok = true;
            break;
        }
        delay(200);
    }

    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    // TZ is clobbered by configTime(); restore ours.
    g_tz_applied = false;
    apply_timezone();
    return ok;
}

}  // namespace rtcclock
