// Voltage -> state of charge, approximately.
//
// There is no fuel gauge on this board, only an ADC across a divider, so the
// percentage is inferred from a discharge curve. Two things make that
// workable here: the reading is always taken at the same point in the boot,
// before any EPD refresh, so the load is consistent; and the device only ever
// discharges, so the result can be held monotonic.
//
// It is still an approximation. A LiPo spends most of its life between 3.8 V
// and 3.95 V, so the middle of the range is inherently coarse -- treat 60%
// and 55% as the same reading. The ends are much more informative.
#pragma once

#include <stdint.h>

namespace battery {

// Above this the pack is being held up by the charger, so the voltage says
// nothing about state of charge.
constexpr uint16_t kChargingMv = 4300;

// Percentage for a resting-ish terminal voltage, clamped to 0..100.
uint8_t percent(uint16_t mv);

// What to actually put on screen. Rounded to the nearest 5, because the cell
// spends most of its life on a flat part of the curve where the observed
// +/-4 mV of ADC noise is worth a couple of points -- showing "63%" would
// imply precision that is not there.
uint8_t display_percent(uint16_t mv);

}  // namespace battery
