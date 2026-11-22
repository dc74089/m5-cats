// Why did the device just power on?
//
// This is the single most load-bearing decision in the firmware: an RTC-driven
// redraw must never be mistaken for a human pressing the button, or the device
// would log a phantom feeding every hour.
//
// The evidence is fragile. BM8563::begin() -- which M5.RTC.begin() calls --
// executes writeReg(0x01, 0x00), wiping the very flags that say "the RTC woke
// you". So read_boot_flags() MUST run before any M5 initialisation.
#pragma once

#include <stdint.h>

namespace wake {

enum class Reason {
    RtcTimer,     // scheduled redraw -- never logs a feeding
    Button,       // human pressed the side button -- this IS the feed event
    External,     // powered on with no button held: USB plug-in, or a press we
                  // somehow missed. Does not auto-log; offers an explicit button.
};

struct BootFlags {
    uint8_t rtc_status2;   // BM8563 Control/Status 2, register 0x01
    bool    rtc_flagged;   // TF (bit 2) or AF (bit 3) set
    bool    button_held;   // GPIO38 still down when we looked
};

// Raise GPIO2 to hold main power on. Must be the first thing that happens in
// setup(): the button only supplies power momentarily, and if the latch is not
// set within a few hundred milliseconds the board dies mid-boot.
void latch_power();

// Reads the RTC status register over raw I2C and samples the button. Safe to
// call only before M5.begin() / M5.RTC.begin().
BootFlags read_boot_flags();

Reason classify(const BootFlags &flags);

const char *reason_name(Reason r);

// Arms the BM8563 countdown so the board powers back on after at least
// `seconds` (see schedule::rtc_timer_for). Replaces M5.shutdown(int), whose
// RTC sequence has two faults: it truncates to whole minutes, so wakes come
// early, and it clears TF while the previous countdown is still running, so
// an expiry in that gap leaves INT asserted and the board unable to power off.
void arm_rtc_wake(uint32_t seconds);

// Raw countdown, for characterising the timer over serial.
void arm_rtc_timer(uint8_t count, bool minutes);

// Stops the countdown and drops the RTC's claim on the power latch.
void disarm_rtc_wake();

// True once the armed countdown has expired (TF set).
bool rtc_wake_fired();

uint8_t rtc_reg(uint8_t reg);

// Drops our hold on main power, with an RTC wake armed after `wake_after_s`
// (0: no wake). Power is held by ANY of GPIO2, the side button, an asserted
// RTC INT, or USB, so this returns only if one of the other three still
// holds it. The caller must NOT re-raise GPIO2 then: doing so is what kept
// the board running on battery until it was flat. See main.cpp loop().
void power_off(uint32_t wake_after_s);

// What GPIO2 is currently driving -- i.e. whether we are holding power on.
bool holding_power();

}  // namespace wake
