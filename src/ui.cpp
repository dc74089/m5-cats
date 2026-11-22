#include "ui.h"

#include <M5EPD.h>
#include <esp_sleep.h>
#include <stdio.h>

#include "Free_Fonts.h"
#include "battery.h"
#include "config.h"

namespace ui {
namespace {

// Canvas greyscale: 0 is white, 15 is black.
constexpr uint8_t kWhite = 0;
constexpr uint8_t kBlack = 15;

constexpr uint8_t kTouchIntPin = 36;
constexpr uint8_t kKeyPushPin  = 38;

constexpr int16_t kBtnY = 395;
constexpr int16_t kBtnW = 300;
constexpr int16_t kBtnH = 105;

M5EPD_Canvas g_canvas(&M5.EPD);

struct Rect {
    int16_t x, y, w, h;
    bool contains(uint16_t px, uint16_t py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

// UNDO sits centred when it is the only button, and to the right when it
// shares the row with FED.
Rect undo_rect(bool with_fed) {
    return Rect{static_cast<int16_t>(with_fed ? 530 : (kScreenW - kBtnW) / 2),
                kBtnY, kBtnW, kBtnH};
}

Rect fed_rect() {
    return Rect{130, kBtnY, kBtnW, kBtnH};
}

void draw_button(M5EPD_Canvas &c, const Rect &r, const char *label) {
    for (int i = 0; i < 3; i++) {
        c.drawRect(r.x + i, r.y + i, r.w - 2 * i, r.h - 2 * i, kBlack);
    }
    c.setFreeFont(FSSB18);
    c.setTextSize(1);
    c.setTextColor(kBlack);
    c.setTextDatum(MC_DATUM);
    c.drawString(label, r.x + r.w / 2, r.y + r.h / 2);
}

// Draws the elapsed text and subtitle at `top`-relative coordinates, so the
// same code serves both the full screen and the standalone band canvas.
void draw_elapsed_block(M5EPD_Canvas &c, int16_t top, const State &s) {
    c.setTextDatum(MC_DATUM);
    c.setTextColor(kBlack);

    // The GFX free fonts stop at 24pt, so doubling is the only way to reach
    // the ~96 px caps that make this readable from across a room.
    c.setFreeFont(FSSB24);
    c.setTextSize(2);
    c.drawString(s.elapsed, kScreenW / 2, top + 70);

    c.setTextSize(1);
    if (s.subtitle[0] != '\0') {
        c.setFreeFont(FSSB18);
        c.drawString(s.subtitle, kScreenW / 2, top + 175);
    }
    if (s.note[0] != '\0') {
        c.setFreeFont(FSSB12);
        c.drawString(s.note, kScreenW / 2, top + 225);
    }
}

}  // namespace

void begin() {
    log_i("psram=%d size=%u free=%u internal_free=%u", psramFound() ? 1 : 0,
          (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram(),
          (unsigned)ESP.getFreeHeap());
    M5.EPD.SetRotation(kRotation);
    M5.TP.SetRotation(kRotation);
}

void clear_panel() {
    M5.EPD.Clear(true);
}

void draw_full(const State &s) {
    if (g_canvas.createCanvas(kScreenW, kScreenH) == nullptr) {
        log_e("full canvas alloc failed (%dx%d, need %d B) -- screen not updated",
              kScreenW, kScreenH, kScreenW * kScreenH / 2);
        return;
    }
    g_canvas.fillCanvas(kWhite);

    g_canvas.setFreeFont(FSSB12);
    g_canvas.setTextSize(1);
    g_canvas.setTextColor(kBlack);
    g_canvas.setTextDatum(TL_DATUM);
    g_canvas.drawString("CATS LAST FED", 40, 34);

    // While the charger is holding the pack up, terminal voltage says nothing
    // about state of charge, so show the state rather than a bogus number.
    char buf[16];
    if (s.on_usb) {
        snprintf(buf, sizeof(buf), "USB");
    } else if (s.battery_mv > 0) {
        snprintf(buf, sizeof(buf), "%u%%", battery::display_percent(s.battery_mv));
    } else {
        buf[0] = '\0';
    }
    if (buf[0] != '\0') {
        g_canvas.setTextDatum(TR_DATUM);
        g_canvas.drawString(buf, kScreenW - 40, 34);
    }
    g_canvas.drawFastHLine(40, 82, kScreenW - 80, 2, kBlack);

    draw_elapsed_block(g_canvas, kElapsedBandY, s);

    if (s.show_fed) {
        draw_button(g_canvas, fed_rect(), "FED THEM");
    }
    if (s.show_undo) {
        draw_button(g_canvas, undo_rect(s.show_fed), "UNDO");
    }

    g_canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
    g_canvas.deleteCanvas();
}

void draw_elapsed_band(const State &s) {
    if (g_canvas.createCanvas(kScreenW, kElapsedBandH) == nullptr) {
        log_e("band canvas alloc failed -- screen not updated");
        return;
    }
    g_canvas.fillCanvas(kWhite);
    draw_elapsed_block(g_canvas, 0, s);
    g_canvas.pushCanvas(0, kElapsedBandY, UPDATE_MODE_GL16);
    g_canvas.deleteCanvas();
}

bool wait_for_undo() {
    M5.TP.flush();
    const Rect undo = undo_rect(/*with_fed=*/false);
    const uint32_t deadline_ms = millis() + kUndoWindowSeconds * 1000;

    bool warned         = false;
    bool use_touch_int  = true;
    int  spurious_ext0  = 0;

    while (static_cast<int32_t>(deadline_ms - millis()) > 0) {
        // Nap rather than spin. 150 ms is imperceptible on a tap but cuts the
        // average draw from ~110 mA to single digits, which is what makes a
        // 60-second undo window affordable at all.
        esp_sleep_enable_timer_wakeup(150ULL * 1000ULL);

        // Touch INT is FALLING-edge (idle high) and the button is active low,
        // so either going low can also break the nap early. These are only an
        // optimisation: the 150 ms timer plus the poll below is what actually
        // guarantees a tap is seen, so failures here are logged, not fatal.
        esp_err_t e0 = ESP_OK;
        if (use_touch_int) {
            e0 = esp_sleep_enable_ext0_wakeup(
                static_cast<gpio_num_t>(kTouchIntPin), 0);
        }
        const esp_err_t e1 =
            esp_sleep_enable_ext1_wakeup(1ULL << kKeyPushPin, ESP_EXT1_WAKEUP_ALL_LOW);
        if (!warned && (e0 != ESP_OK || e1 != ESP_OK)) {
            warned = true;
            log_w("undo wake sources unavailable (ext0=%d ext1=%d); polling only",
                  e0, e1);
        }

        esp_light_sleep_start();
        const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

        // Poll regardless of what woke us: the GT911 interrupt is a short
        // pulse and can be missed, but the finger state persists.
        M5.TP.update();
        const bool finger_down = !M5.TP.isFingerUp();

        if (finger_down) {
            const uint16_t x = M5.TP.readFingerX(0);
            const uint16_t y = M5.TP.readFingerY(0);
            log_i("touch %u,%u (undo x %d-%d y %d-%d) hit=%d", x, y, undo.x,
                  undo.x + undo.w, undo.y, undo.y + undo.h,
                  undo.contains(x, y) ? 1 : 0);
            if (undo.contains(x, y)) {
                return true;
            }
        }

        // If the touch INT line turns out to idle low, ext0 would return from
        // light sleep instantly every iteration and silently turn this window
        // into a full-power busy-wait -- the exact cost this is meant to
        // avoid. Detect that and fall back to the timer alone.
        if (use_touch_int && cause == ESP_SLEEP_WAKEUP_EXT0 && !finger_down) {
            if (++spurious_ext0 >= 5) {
                use_touch_int = false;
                log_w("touch INT idles low; disabling ext0 wake for this window");
            }
        } else if (finger_down) {
            spurious_ext0 = 0;
        }
    }
    return false;
}

Tap poll_tap(const State &s) {
    M5.TP.update();
    if (M5.TP.isFingerUp()) {
        return Tap::None;
    }
    const uint16_t x = M5.TP.readFingerX(0);
    const uint16_t y = M5.TP.readFingerY(0);
    if (s.show_undo && undo_rect(s.show_fed).contains(x, y)) {
        return Tap::Undo;
    }
    if (s.show_fed && fed_rect().contains(x, y)) {
        return Tap::Fed;
    }
    return Tap::None;
}

uint32_t wait_for_refresh() {
    const uint32_t started = millis();
    M5.EPD.CheckAFSR();  // polls LUTAFSR until the display engine goes idle
    // Small margin on top of the register going clear, because the failure
    // mode on the other side is a visibly corrupted panel.
    delay(50);
    return millis() - started;
}

void draw_message(const char *line1, const char *line2) {
    g_canvas.createCanvas(kScreenW, kScreenH);
    g_canvas.fillCanvas(kWhite);

    g_canvas.setTextDatum(MC_DATUM);
    g_canvas.setTextColor(kBlack);
    g_canvas.setFreeFont(FSSB24);
    g_canvas.setTextSize(1);
    g_canvas.drawString(line1, kScreenW / 2, kScreenH / 2 - 40);

    if (line2 != nullptr && line2[0] != '\0') {
        g_canvas.setFreeFont(FSSB18);
        g_canvas.drawString(line2, kScreenW / 2, kScreenH / 2 + 40);
    }

    g_canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
    g_canvas.deleteCanvas();
}

void draw_diagnostics(uint32_t boots, time_t last_fed, time_t last_sync,
                      uint16_t battery_mv, const char *wake_reason) {
    g_canvas.createCanvas(kScreenW, kScreenH);
    g_canvas.fillCanvas(kWhite);
    g_canvas.setTextColor(kBlack);
    g_canvas.setTextDatum(TL_DATUM);
    g_canvas.setFreeFont(FSSB18);
    g_canvas.setTextSize(1);
    g_canvas.drawString("Diagnostics", 40, 30);

    g_canvas.setFreeFont(FSSB12);
    char buf[96];
    int16_t y = 110;
    constexpr int16_t kStep = 46;

    snprintf(buf, sizeof(buf), "boots: %u", boots);
    g_canvas.drawString(buf, 40, y); y += kStep;
    snprintf(buf, sizeof(buf), "battery: %u mV (~%u%%)", battery_mv,
             battery::display_percent(battery_mv));
    g_canvas.drawString(buf, 40, y); y += kStep;
    snprintf(buf, sizeof(buf), "woke by: %s", wake_reason);
    g_canvas.drawString(buf, 40, y); y += kStep;
    snprintf(buf, sizeof(buf), "last fed (epoch): %lld", (long long)last_fed);
    g_canvas.drawString(buf, 40, y); y += kStep;
    snprintf(buf, sizeof(buf), "last sync (epoch): %lld", (long long)last_sync);
    g_canvas.drawString(buf, 40, y); y += kStep;
    g_canvas.drawString("USB: pio device monitor -- type 'help'", 40, y);

    g_canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
    g_canvas.deleteCanvas();
}

}  // namespace ui
