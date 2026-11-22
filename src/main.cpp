// Cats last fed -- M5Paper.
//
// The device is powered off almost all of the time. Each boot is a complete
// lifecycle: work out why we woke, update the screen, decide when the display
// will next go stale, and cut power until then. setup() runs that lifecycle
// end to end and normally never returns -- wake::power_off() de-latches GPIO2
// and the board dies mid-function.
//
// Execution continues past power_off() only while something else holds
// power: normally the USB cable, but a held side button or a stuck RTC INT
// do it too, and from code they are indistinguishable. So loop() never takes
// the latch back. It keeps GPIO2 low and re-arms the RTC every second, and
// the board turns itself off the moment the cable (or whatever it was) goes.
// Re-raising GPIO2 here once kept a unit running on battery until it was
// flat: about two days, against a budget of months.

#include <Arduino.h>
#include <M5EPD.h>
#include <stdio.h>
#include <string.h>

#include "battery.h"
#include "clock.h"
#include "config.h"
#include "schedule.h"
#include "store.h"
#include "ui.h"
#include "wake.h"

namespace {

constexpr uint8_t kKeyLeftPin = 37;

// Set once we know the cable is in and shutdown is impossible.
bool g_interactive = false;

char g_elapsed[48];
char g_subtitle[48];

// Falls back to the build time so a fresh unit shows something sane before
// its first successful NTP sync. BUILD_EPOCH_UTC is injected by
// tools/build_timestamp.py; parsing __DATE__/__TIME__ instead would give the
// build machine's local time and seed the RTC hours off.
time_t build_time_utc() {
#ifdef BUILD_EPOCH_UTC
    return static_cast<time_t>(BUILD_EPOCH_UTC);
#else
    return static_cast<time_t>(0);
#endif
}

// Syncs the clock and carries stored feeding times along with any correction.
// Without this, a large step (typically the first sync after a build-time
// seed) would silently change how long ago the cats were last fed.
bool sync_and_reconcile() {
    const time_t before     = rtcclock::now_utc();
    const time_t prior_sync = store::last_sync();
    if (!rtcclock::sync_ntp()) {
        return false;
    }
    const time_t after = rtcclock::now_utc();
    const int64_t delta = static_cast<int64_t>(after) - static_cast<int64_t>(before);

    // Ignore ordinary drift; only reconcile genuine steps. In steady state
    // this never fires -- it is for the first sync after a build-time seed.
    if (delta > 120 || delta < -120) {
        const size_t moved = store::shift_feedings_after(prior_sync, delta);
        Serial.printf("clock stepped %lld s; %u feeding time(s) shifted\n",
                      (long long)delta, (unsigned)moved);
    }
    store::set_last_sync(after);
    return true;
}

// Builds the current screen contents from stored state.
ui::State compose(time_t now, bool show_undo, bool show_fed, uint16_t mv,
                  const char *note, uint32_t *next_change_s) {
    const time_t fed = store::last_fed();

    ui::State s;
    s.battery_mv = mv;
    // show_fed is only ever set in the USB interactive loop.
    s.on_usb     = show_fed;
    s.show_undo  = show_undo;
    s.show_fed   = show_fed;
    s.note       = note;

    if (fed == 0) {
        snprintf(g_elapsed, sizeof(g_elapsed), "not yet fed");
        snprintf(g_subtitle, sizeof(g_subtitle), "press the side button");
        // Nothing will change on its own; check back at the maximum interval.
        *next_change_s = schedule::kMaxSleepSeconds;
    } else {
        const schedule::Rendered r = schedule::describe_elapsed(now - fed);
        snprintf(g_elapsed, sizeof(g_elapsed), "%s", r.text);
        *next_change_s = r.next_change_s;

        // Within today, the clock time alone is unambiguous; past that, say
        // which day it was.
        if (now - fed < 20 * 3600) {
            rtcclock::format_local_time(fed, g_subtitle, sizeof(g_subtitle));
        } else {
            rtcclock::format_local_day_time(fed, g_subtitle, sizeof(g_subtitle));
        }
    }

    s.elapsed  = g_elapsed;
    s.subtitle = g_subtitle;
    return s;
}

void render(const ui::State &s, bool force_full) {
    // A layout change can leave pixels the new layout never writes over, so
    // wipe the panel once before the first draw of a new revision.
    if (store::ui_rev() != kUiRev) {
        ui::clear_panel();
        store::set_ui_rev(kUiRev);
        force_full = true;
    }

    const bool full = force_full || store::should_full_refresh();
    if (full) {
        ui::draw_full(s);
    } else {
        ui::draw_elapsed_band(s);
    }
    store::note_refresh(full);
}

// Called when power_off() returned, i.e. the board is still powered by
// something other than GPIO2.
void enter_interactive() {
    Serial.println("still powered -> interactive mode (USB, or button held); "
                   "will drop off as soon as nothing holds power");
    // Deliberately NOT M5.enableMainPower(). With GPIO2 high the board stays
    // up on battery after the cable is pulled, running this loop flat out.
    store::begin();
    // The USB screen draws buttons below the partial-refresh band, so the
    // first scheduled wake after unplugging has to repaint everything.
    store::force_full_refresh();
    g_interactive = true;
}

void dump_battery_log() {
    store::BatterySample samples[kBatteryRingSize];
    const size_t n = store::read_battery_log(samples, kBatteryRingSize);
    Serial.printf("# battery log: %u samples (oldest first)\n", (unsigned)n);
    Serial.println("epoch_utc,millivolts");
    for (size_t i = 0; i < n; i++) {
        Serial.printf("%u,%u\n", samples[i].at, samples[i].mv);
    }
}

}  // namespace

void setup() {
    // Nothing may precede this. The side button supplies power only
    // momentarily; if GPIO2 is not driven high within a few hundred
    // milliseconds the board loses power mid-boot.
    wake::latch_power();
    setCpuFrequencyMhz(kCpuMhz);

    // Must happen before M5.begin(): M5.RTC.begin() writes 0x00 to BM8563
    // register 0x01, destroying the flags that say the RTC woke us.
    const wake::BootFlags flags  = wake::read_boot_flags();
    const wake::Reason    reason = wake::classify(flags);

    pinMode(kKeyLeftPin, INPUT);
    const bool diagnostics = digitalRead(kKeyLeftPin) == LOW;

    M5.begin(/*touchEnable=*/true, /*SDEnable=*/false, /*SerialEnable=*/true,
             /*BatteryADCEnable=*/true, /*I2CEnable=*/false);
    rtcclock::begin();
    ui::begin();
    store::begin();

    const uint32_t boots = store::increment_boot_count();
    const uint16_t mv    = static_cast<uint16_t>(M5.getBatteryVoltage());

    Serial.printf("\n[boot %u] wake=%s status2=0x%02X btn=%d bat=%umV (~%u%%)\n",
                  boots, wake::reason_name(reason), flags.rtc_status2,
                  flags.button_held ? 1 : 0, mv,
                  battery::display_percent(static_cast<uint16_t>(mv)));

    if (!rtcclock::is_set()) {
        Serial.println("RTC unset; attempting NTP");
        store::set_last_sync_attempt(rtcclock::now_utc());
        if (sync_and_reconcile()) {
            Serial.println("RTC set from NTP");
        } else {
            rtcclock::set_utc(build_time_utc());
            Serial.println("NTP failed; RTC seeded from build time");
        }
    }

    time_t now = rtcclock::now_utc();
    store::maybe_log_battery(now, mv);

    if (diagnostics) {
        dump_battery_log();
        ui::draw_diagnostics(boots, store::last_fed(), store::last_sync(), mv,
                             wake::reason_name(reason));
        store::end();
        ui::wait_for_refresh();
        // Park: leave the diagnostics on screen until the user power-cycles.
        wake::power_off(/*wake_after_s=*/0);
        enter_interactive();
        return;
    }

    bool logged     = false;
    bool offer_undo = false;
    if (reason == wake::Reason::Button) {
        logged = store::record_feeding(now);
        // Offer undo on any button press with something to undo, not only on
        // a press that logged. Otherwise pressing again just after the window
        // closed is a dead end: the press is deduped, no window opens, and
        // there is no way back off-cable.
        offer_undo = store::last_fed() != 0;
        Serial.printf("button press -> %s\n",
                      logged ? "feeding logged" : "ignored (duplicate)");
    }

    // Only sync on scheduled wakes. Doing it on a feeding would stall the
    // undo window behind a WiFi connect.
    if (reason == wake::Reason::RtcTimer &&
        rtcclock::sync_due(store::last_sync(), store::last_sync_attempt())) {
        Serial.println("clock sync due");
        store::set_last_sync_attempt(now);
        if (sync_and_reconcile()) {
            now = rtcclock::now_utc();
            Serial.println("clock synced");
        } else {
            Serial.println("clock sync failed (ignored)");
        }
    }

    uint32_t next_change = schedule::kMaxSleepSeconds;
    ui::State s = compose(now, /*show_undo=*/offer_undo, /*show_fed=*/false, mv,
                          "", &next_change);
    render(s, /*force_full=*/offer_undo);

    if (offer_undo) {
        if (ui::wait_for_undo()) {
            store::undo_feeding();
            Serial.println("undone");
        }
        now = rtcclock::now_utc();
        s   = compose(now, /*show_undo=*/false, /*show_fed=*/false, mv, "",
                      &next_change);
        render(s, /*force_full=*/true);
    }

    const uint32_t sleep_s = schedule::clamp_sleep(next_change);

    // The panel refreshes asynchronously, so power must not be cut until it
    // has finished -- otherwise the update is left half-drawn.
    const uint32_t waited_ms = ui::wait_for_refresh();

    Serial.printf("display settled in %u ms; sleeping %u s (next text change)\n",
                  waited_ms, sleep_s);
    Serial.flush();
    store::end();

    wake::power_off(sleep_s);

    // Still here: power is held by something other than GPIO2 -- usually
    // USB, possibly a held button. loop() lets go the moment it stops.
    enter_interactive();
}

void handle_serial() {
    if (!Serial.available()) {
        return;
    }
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();

    if (cmd == "help") {
        Serial.println(
            "commands: help, dump, state, sync, skew <secs>, bat, "
            "undotest, rtctest <secs|Nm|Ns>, fed <secs-ago>, undo, reset");
    } else if (cmd == "dump") {
        dump_battery_log();
    } else if (cmd == "state") {
        const time_t now = rtcclock::now_utc();
        const time_t fed = store::last_fed();
        Serial.printf("now=%lld last_fed=%lld prev=%lld elapsed=%lld\n",
                      (long long)now, (long long)fed,
                      (long long)store::prev_fed(),
                      (long long)(fed ? now - fed : -1));
        Serial.printf("last_sync=%lld last_attempt=%lld sync_due=%d\n",
                      (long long)store::last_sync(),
                      (long long)store::last_sync_attempt(),
                      rtcclock::sync_due(store::last_sync(),
                                         store::last_sync_attempt()) ? 1 : 0);
        if (fed) {
            const schedule::Rendered r = schedule::describe_elapsed(now - fed);
            Serial.printf("text=\"%s\" next_change=%u s\n", r.text,
                          r.next_change_s);
        }
        // Everything that can hold the board on, bar USB itself.
        const uint8_t s2 = wake::rtc_reg(0x01);
        Serial.printf("power: gpio2_latch=%d button=%d rtc_status2=0x%02X "
                      "(TIE=%d TF=%d) timer_ctrl=0x%02X count=%u clock_lost=%d\n",
                      wake::holding_power() ? 1 : 0,
                      digitalRead(38) == LOW ? 1 : 0, s2, s2 & 1, (s2 >> 2) & 1,
                      wake::rtc_reg(0x0E), wake::rtc_reg(0x0F),
                      rtcclock::clock_integrity_lost() ? 1 : 0);
    } else if (cmd.startsWith("fed ")) {
        // Backdates the last feeding, for walking the display ladder over USB.
        const long ago = cmd.substring(4).toInt();
        store::record_feeding(rtcclock::now_utc() - ago);
        Serial.printf("last_fed set to %ld s ago\n", ago);
    } else if (cmd == "undotest") {
        // Exercises the light-sleep undo window on USB, where serial is
        // observable -- on battery this path runs blind.
        Serial.println("undo window open; tap UNDO...");
        const bool hit = ui::wait_for_undo();
        Serial.printf("undo window closed: %s\n", hit ? "TAPPED" : "timed out");
    } else if (cmd == "bat") {
        // The sense divider only sees the cell while GPIO2 is high, so take
        // the latch for the duration of the reading and no longer. Pulling
        // the cable mid-reading keeps the board up for about a second.
        M5.enableMainPower();
        delay(20);
        // Repeated samples: exposes both a wrong scale and random noise.
        for (int i = 0; i < 8; i++) {
            const uint16_t v = static_cast<uint16_t>(M5.getBatteryVoltage());
            Serial.printf("raw=%u mv=%u pct=%u disp=%u\n", M5.getBatteryRaw(), v,
                          battery::percent(v), battery::display_percent(v));
            delay(120);
        }
        M5.disableMainPower();
    } else if (cmd.startsWith("rtctest ")) {
        // Times the RTC countdown, which on battery is never observable:
        // "rtctest 3599" uses the production rounding, "rtctest 2m" / "5s"
        // a raw count of minute / second ticks.
        String arg = cmd.substring(8);
        arg.trim();
        const long n = arg.toInt();
        schedule::RtcTimer t;
        if (arg.endsWith("m") || arg.endsWith("s")) {
            t = schedule::RtcTimer{static_cast<uint8_t>(n), arg.endsWith("m")};
        } else {
            t = schedule::rtc_timer_for(static_cast<uint32_t>(n));
        }
        rtc_time_t at;
        M5.RTC.getTime(&at);
        wake::arm_rtc_timer(t.count, t.minutes);
        const uint32_t started = millis();
        Serial.printf("armed %u x %s (expect %u-%u s) at rtc :%02d\n", t.count,
                      t.minutes ? "1/60Hz" : "1Hz", schedule::rtc_timer_min_delay(t),
                      schedule::rtc_timer_max_delay(t), at.sec);
        const uint32_t limit_ms = (schedule::rtc_timer_max_delay(t) + 90) * 1000;
        while (!wake::rtc_wake_fired() && millis() - started < limit_ms) {
            delay(20);
        }
        M5.RTC.getTime(&at);
        if (wake::rtc_wake_fired()) {
            Serial.printf("rtc timer fired after %u ms at rtc :%02d\n",
                          millis() - started, at.sec);
        } else {
            Serial.println("rtc timer did NOT fire");
        }
    } else if (cmd == "sync") {
        // Bypasses the retry backoff, so new credentials can be checked
        // immediately instead of waiting out kSyncRetryIntervalSeconds.
        Serial.println("forcing NTP sync...");
        store::set_last_sync_attempt(rtcclock::now_utc());
        if (sync_and_reconcile()) {
            char when[48];
            rtcclock::format_local_day_time(rtcclock::now_utc(), when, sizeof(when));
            Serial.printf("synced; local time is now %s\n", when);
        } else {
            Serial.println("sync FAILED (check include/secrets.h)");
        }
    } else if (cmd.startsWith("skew ")) {
        // Deliberately mis-set the RTC so the next sync has to step it back,
        // exercising the reconciliation path.
        const long by = cmd.substring(5).toInt();
        rtcclock::set_utc(rtcclock::now_utc() + by);
        Serial.printf("RTC skewed by %ld s\n", by);
    } else if (cmd == "undo") {
        Serial.println(store::undo_feeding() ? "undone" : "nothing to undo");
    } else if (cmd == "reset") {
        Serial.println("clearing feeding history");
        store::clear_feedings();
    } else if (cmd.length() > 0) {
        Serial.printf("unknown command: %s (try 'help')\n", cmd.c_str());
    }
}

void loop() {
    if (!g_interactive) {
        delay(1000);
        return;
    }

    static uint32_t last_render = 0;
    static uint32_t last_arm    = 0;
    static char     last_text[48] = {};

    // Re-arming clears TF, so a stale RTC interrupt can never be what keeps
    // the board up, and restarts the countdown so it never fires while the
    // cable is in. Together with GPIO2 staying low, that leaves USB (or a
    // held button) as the only thing holding power: pull it and the board is
    // off within milliseconds, waking kUsbExitWakeSeconds later.
    if (last_arm == 0 || millis() - last_arm >= 1000) {
        wake::arm_rtc_wake(kUsbExitWakeSeconds);
        last_arm = millis();
    }

    handle_serial();

    // No battery reading here: the sense divider hangs off GPIO2, which stays
    // low, and the header shows "USB" rather than a percentage anyway.
    const uint16_t mv  = 0;
    const time_t   now = rtcclock::now_utc();
    uint32_t       next_change = 0;
    ui::State s = compose(now, /*show_undo=*/store::last_fed() != 0,
                          /*show_fed=*/true, mv, "USB connected", &next_change);

    const ui::Tap tap = ui::poll_tap(s);
    bool dirty = false;

    if (tap == ui::Tap::Fed) {
        Serial.println(store::record_feeding(now) ? "feeding logged"
                                                  : "ignored (duplicate)");
        dirty = true;
    } else if (tap == ui::Tap::Undo) {
        Serial.println(store::undo_feeding() ? "undone" : "nothing to undo");
        dirty = true;
    }

    // Repaint when the text actually differs, or at most once a minute, so
    // the panel is not driven continuously while plugged in.
    if (dirty || strcmp(last_text, s.elapsed) != 0 ||
        millis() - last_render > 60000) {
        s = compose(rtcclock::now_utc(), store::last_fed() != 0, true, mv,
                    "USB connected", &next_change);
        ui::draw_full(s);
        snprintf(last_text, sizeof(last_text), "%s", s.elapsed);
        last_render = millis();
        if (dirty) {
            delay(600);  // debounce: avoid one tap registering twice
            M5.TP.flush();
        }
    }

    delay(100);
}
