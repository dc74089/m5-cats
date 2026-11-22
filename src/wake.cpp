#include "wake.h"

#include <Arduino.h>
#include <Wire.h>
#include <soc/gpio_reg.h>

#include "schedule.h"

namespace wake {
namespace {

constexpr uint8_t kBm8563Addr    = 0x51;
constexpr uint8_t kRegStatus2    = 0x01;
constexpr uint8_t kRegTimerCtrl  = 0x0E;
constexpr uint8_t kRegTimerCount = 0x0F;

// Control/Status 2 bit layout. TI_TP (bit 4) is left 0, so INT is a level
// that stays asserted for as long as TF is set -- which is why a stale TF
// holds the power latch on.
constexpr uint8_t kTimerIntEnable = 1 << 0;  // TIE
constexpr uint8_t kTimerFlag      = 1 << 2;  // TF
constexpr uint8_t kAlarmFlag      = 1 << 3;  // AF

// Timer control: TE in bit 7, source clock in bits 1:0.
constexpr uint8_t kTimerEnable   = 1 << 7;
constexpr uint8_t kTimerSrc1Hz   = 0x02;
constexpr uint8_t kTimerSrcMin   = 0x03;  // 1/60 Hz; also the low-power idle setting

constexpr uint8_t kMainPwrPin  = 2;
constexpr uint8_t kKeyPushPin  = 38;
constexpr uint8_t kI2cSdaPin   = 21;
constexpr uint8_t kI2cSclPin   = 22;

void write_reg(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(kBm8563Addr);
    Wire.write(reg);
    Wire.write(value);
    Wire.endTransmission();
}

}  // namespace

void latch_power() {
    pinMode(kMainPwrPin, OUTPUT);
    digitalWrite(kMainPwrPin, HIGH);
}

BootFlags read_boot_flags() {
    BootFlags flags{};

    // The button is read first and without a debounce delay. We are only a few
    // milliseconds past the earliest moment code can run, and the board could
    // not have booted at all unless the button was held at least that long, so
    // a genuine press is almost certainly still down.
    pinMode(kKeyPushPin, INPUT);
    flags.button_held = digitalRead(kKeyPushPin) == LOW;

    Wire.begin(kI2cSdaPin, kI2cSclPin);
    Wire.beginTransmission(kBm8563Addr);
    Wire.write(kRegStatus2);
    if (Wire.endTransmission() != 0) {
        // RTC unreachable. Fall back to treating this as external so we do not
        // invent a feeding out of an I2C fault.
        flags.rtc_status2 = 0;
        flags.rtc_flagged = false;
        return flags;
    }

    Wire.requestFrom(static_cast<int>(kBm8563Addr), 1);
    flags.rtc_status2 = Wire.available() ? Wire.read() : 0;
    flags.rtc_flagged = (flags.rtc_status2 & (kTimerFlag | kAlarmFlag)) != 0;
    return flags;
}

Reason classify(const BootFlags &flags) {
    // The RTC flag is authoritative and covers the common case (~27 scheduled
    // wakes a day against ~3 feedings), so checking it first keeps the
    // expensive mistake -- a phantom feeding -- off the likely path.
    if (flags.rtc_flagged) {
        return Reason::RtcTimer;
    }
    if (flags.button_held) {
        return Reason::Button;
    }
    // Powered on by something that is not the RTC and not a held button.
    // Overwhelmingly this is a USB plug-in. Deliberately does not log.
    return Reason::External;
}

const char *reason_name(Reason r) {
    switch (r) {
        case Reason::RtcTimer: return "rtc-timer";
        case Reason::Button:   return "button";
        case Reason::External: return "external/usb";
    }
    return "?";
}

uint8_t rtc_reg(uint8_t reg) {
    Wire.beginTransmission(kBm8563Addr);
    Wire.write(reg);
    if (Wire.endTransmission() != 0) {
        return 0;
    }
    Wire.requestFrom(static_cast<int>(kBm8563Addr), 1);
    return Wire.available() ? Wire.read() : 0;
}

void arm_rtc_timer(uint8_t count, bool minutes) {
    // Order matters. The countdown from the previous sleep is still running
    // (it auto-reloads, and M5.RTC.begin() only cleared TIE), so stop it
    // first: then nothing can set TF between clearing it and enabling TIE.
    write_reg(kRegTimerCtrl, kTimerSrcMin);       // TE=0
    write_reg(kRegStatus2, 0x00);                 // TF=AF=0, TIE=AIE=0
    write_reg(kRegTimerCount, count);
    write_reg(kRegTimerCtrl,
              kTimerEnable | (minutes ? kTimerSrcMin : kTimerSrc1Hz));
    write_reg(kRegStatus2, kTimerIntEnable);
}

void arm_rtc_wake(uint32_t seconds) {
    const schedule::RtcTimer t = schedule::rtc_timer_for(seconds);
    arm_rtc_timer(t.count, t.minutes);
}

void disarm_rtc_wake() {
    write_reg(kRegTimerCtrl, kTimerSrcMin);
    write_reg(kRegStatus2, 0x00);
}

bool rtc_wake_fired() {
    return (rtc_reg(kRegStatus2) & kTimerFlag) != 0;
}

void power_off(uint32_t wake_after_s) {
    if (wake_after_s > 0) {
        arm_rtc_wake(wake_after_s);
    } else {
        disarm_rtc_wake();
    }
    digitalWrite(kMainPwrPin, LOW);
    // On battery the rail collapses within milliseconds. Surviving this long
    // means another source is holding power.
    delay(500);
}

bool holding_power() {
    return (REG_READ(GPIO_OUT_REG) >> kMainPwrPin) & 1;
}

}  // namespace wake
