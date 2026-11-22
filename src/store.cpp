#include "store.h"

#include <Preferences.h>
#include <string.h>

#include "config.h"

namespace store {
namespace {

Preferences prefs;

constexpr const char *kNamespace = "cats";

constexpr const char *kKeyLastFed    = "last_fed";
constexpr const char *kKeyPrevFed    = "prev_fed";
constexpr const char *kKeyLastSync   = "last_sync";
constexpr const char *kKeySyncTry    = "sync_try";
constexpr const char *kKeyRefreshes  = "refreshes";
constexpr const char *kKeyBoots      = "boots";
constexpr const char *kKeyUiRev      = "ui_rev";
constexpr const char *kKeyBatRing    = "bat_ring";
constexpr const char *kKeyBatHead    = "bat_head";
constexpr const char *kKeyBatCount   = "bat_count";
constexpr const char *kKeyBatLast    = "bat_last";

}  // namespace

void begin() {
    prefs.begin(kNamespace, /*readOnly=*/false);
}

void end() {
    prefs.end();
}

time_t last_fed() {
    return static_cast<time_t>(prefs.getUInt(kKeyLastFed, 0));
}

time_t prev_fed() {
    return static_cast<time_t>(prefs.getUInt(kKeyPrevFed, 0));
}

bool record_feeding(time_t at) {
    const time_t previous = last_fed();
    if (previous != 0 && at >= previous &&
        static_cast<uint32_t>(at - previous) < kDedupeSeconds) {
        return false;
    }
    prefs.putUInt(kKeyPrevFed, static_cast<uint32_t>(previous));
    prefs.putUInt(kKeyLastFed, static_cast<uint32_t>(at));
    return true;
}

bool undo_feeding() {
    const time_t previous = prev_fed();
    if (last_fed() == 0) {
        return false;
    }
    prefs.putUInt(kKeyLastFed, static_cast<uint32_t>(previous));
    // Undo is single level: once used, there is nothing further back to go to.
    prefs.putUInt(kKeyPrevFed, 0);
    return true;
}

void clear_feedings() {
    prefs.putUInt(kKeyLastFed, 0);
    prefs.putUInt(kKeyPrevFed, 0);
}

size_t shift_feedings_after(time_t boundary, int64_t delta) {
    size_t moved = 0;
    const time_t last = last_fed();
    const time_t prev = prev_fed();
    if (last != 0 && last > boundary) {
        prefs.putUInt(kKeyLastFed, static_cast<uint32_t>(last + delta));
        moved++;
    }
    if (prev != 0 && prev > boundary) {
        prefs.putUInt(kKeyPrevFed, static_cast<uint32_t>(prev + delta));
        moved++;
    }
    return moved;
}

bool should_full_refresh() {
    return prefs.getUInt(kKeyRefreshes, 0) >= kFullRefreshEvery;
}

void note_refresh(bool was_full) {
    if (was_full) {
        prefs.putUInt(kKeyRefreshes, 0);
    } else {
        prefs.putUInt(kKeyRefreshes, prefs.getUInt(kKeyRefreshes, 0) + 1);
    }
}

void force_full_refresh() {
    prefs.putUInt(kKeyRefreshes, kFullRefreshEvery);
}

time_t last_sync() {
    return static_cast<time_t>(prefs.getUInt(kKeyLastSync, 0));
}

void set_last_sync(time_t at) {
    prefs.putUInt(kKeyLastSync, static_cast<uint32_t>(at));
}

time_t last_sync_attempt() {
    return static_cast<time_t>(prefs.getUInt(kKeySyncTry, 0));
}

void set_last_sync_attempt(time_t at) {
    prefs.putUInt(kKeySyncTry, static_cast<uint32_t>(at));
}

uint32_t ui_rev() {
    return prefs.getUInt(kKeyUiRev, 0);
}

void set_ui_rev(uint32_t rev) {
    prefs.putUInt(kKeyUiRev, rev);
}

uint32_t boot_count() {
    return prefs.getUInt(kKeyBoots, 0);
}

uint32_t increment_boot_count() {
    const uint32_t next = boot_count() + 1;
    prefs.putUInt(kKeyBoots, next);
    return next;
}

void maybe_log_battery(time_t now, uint16_t mv) {
    const uint32_t last = prefs.getUInt(kKeyBatLast, 0);
    if (last != 0 && static_cast<uint32_t>(now) - last < kBatterySampleIntervalSeconds) {
        return;
    }

    BatterySample ring[kBatteryRingSize];
    memset(ring, 0, sizeof(ring));
    prefs.getBytes(kKeyBatRing, ring, sizeof(ring));

    uint32_t head  = prefs.getUInt(kKeyBatHead, 0) % kBatteryRingSize;
    uint32_t count = prefs.getUInt(kKeyBatCount, 0);

    ring[head] = BatterySample{static_cast<uint32_t>(now), mv};
    head       = (head + 1) % kBatteryRingSize;
    if (count < kBatteryRingSize) {
        count++;
    }

    prefs.putBytes(kKeyBatRing, ring, sizeof(ring));
    prefs.putUInt(kKeyBatHead, head);
    prefs.putUInt(kKeyBatCount, count);
    prefs.putUInt(kKeyBatLast, static_cast<uint32_t>(now));
}

size_t read_battery_log(BatterySample *out, size_t max) {
    BatterySample ring[kBatteryRingSize];
    memset(ring, 0, sizeof(ring));
    prefs.getBytes(kKeyBatRing, ring, sizeof(ring));

    const uint32_t head  = prefs.getUInt(kKeyBatHead, 0) % kBatteryRingSize;
    const uint32_t count = prefs.getUInt(kKeyBatCount, 0);

    const size_t n     = count < max ? count : max;
    // Oldest entry sits `count` slots behind the head.
    const uint32_t start = (head + kBatteryRingSize - count) % kBatteryRingSize;
    for (size_t i = 0; i < n; i++) {
        out[i] = ring[(start + i) % kBatteryRingSize];
    }
    return n;
}

}  // namespace store
