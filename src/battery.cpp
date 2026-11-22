#include "battery.h"

#include <stddef.h>

namespace battery {
namespace {

// Discharge curve for the 4.35 V high-voltage LiPo M5Stack fits to the
// M5Paper, measured at light load. Descending by voltage.
//
// 0% is pinned at 3.40 V rather than the cell's true floor: below that the
// curve falls off a cliff, and there is no value in reporting single-digit
// percentages that are about to vanish anyway.
struct Point {
    uint16_t mv;
    uint8_t  pct;
};

constexpr Point kCurve[] = {
    {4350, 100}, {4180, 90}, {4030, 80}, {3935, 70}, {3875, 60}, {3830, 50},
    {3795, 40},  {3765, 30}, {3735, 20}, {3695, 10}, {3400, 0},
};

constexpr size_t kCurveLen = sizeof(kCurve) / sizeof(kCurve[0]);

}  // namespace

uint8_t percent(uint16_t mv) {
    if (mv >= kCurve[0].mv) {
        return 100;
    }
    if (mv <= kCurve[kCurveLen - 1].mv) {
        return 0;
    }

    for (size_t i = 1; i < kCurveLen; i++) {
        if (mv >= kCurve[i].mv) {
            const Point &hi = kCurve[i - 1];
            const Point &lo = kCurve[i];
            // Linear interpolation between the bracketing points.
            const int32_t span_mv  = hi.mv - lo.mv;
            const int32_t span_pct = hi.pct - lo.pct;
            const int32_t into     = mv - lo.mv;
            return static_cast<uint8_t>(lo.pct + (into * span_pct + span_mv / 2) /
                                                     span_mv);
        }
    }
    return 0;
}

uint8_t display_percent(uint16_t mv) {
    return static_cast<uint8_t>(((percent(mv) + 2) / 5) * 5);
}

}  // namespace battery
