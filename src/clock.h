// RTC access, timezone handling and the weekly NTP sync.
//
// The RTC is held in UTC and converted for display with a POSIX TZ string, so
// a DST change needs no reflash. Elapsed time is a difference between two
// reads of the same clock, so drift does not affect the headline number --
// NTP only keeps the "(7:42 am)" subtitle honest.
#pragma once

#include <stddef.h>
#include <time.h>

namespace rtcclock {

void begin();

// False if the RTC has lost power (VL set) or reads an implausible year.
bool is_set();

// The RTC's own record that its oscillator stopped since the time was set.
bool clock_integrity_lost();

time_t now_utc();
void   set_utc(time_t t);

// Calendar -> epoch, always interpreted as UTC. Unlike mktime() this does not
// depend on the active TZ, which matters for the build-time fallback.
time_t epoch_from_utc(int year, int mon, int day, int hour, int min, int sec);

// "7:42 am", in local time.
void format_local_time(time_t t, char *buf, size_t n);
// "Sat 7:42 am", used when the feeding was not today.
void format_local_day_time(time_t t, char *buf, size_t n);

// True if a sync is both wanted and allowed: kSyncIntervalSeconds since the
// last success, and at least kSyncRetryIntervalSeconds since the last attempt.
bool sync_due(time_t last_sync, time_t last_attempt);

// Brings up WiFi, queries NTP, writes the RTC, tears WiFi back down.
// Returns false on any failure; failure is never fatal.
bool sync_ntp();

}  // namespace rtcclock
