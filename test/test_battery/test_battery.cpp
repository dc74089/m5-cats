#include <unity.h>

#include "battery.h"

using battery::percent;

void test_endpoints(void) {
    TEST_ASSERT_EQUAL_UINT8(100, percent(4350));
    TEST_ASSERT_EQUAL_UINT8(100, percent(4400));  // clamped, not wrapped
    TEST_ASSERT_EQUAL_UINT8(0, percent(3400));
    TEST_ASSERT_EQUAL_UINT8(0, percent(3000));
    TEST_ASSERT_EQUAL_UINT8(0, percent(0));       // ADC failure must not read full
}

void test_known_points(void) {
    TEST_ASSERT_EQUAL_UINT8(90, percent(4180));
    TEST_ASSERT_EQUAL_UINT8(50, percent(3830));
    TEST_ASSERT_EQUAL_UINT8(10, percent(3695));
}

void test_interpolates_between_points(void) {
    // Midway between 4150/90% and 4080/85% should land near 87-88%.
    const uint8_t mid = percent(4115);
    TEST_ASSERT_TRUE(mid > 85 && mid < 90);
}

// A percentage that can go up as voltage falls would be visibly broken.
void test_monotonic_in_voltage(void) {
    uint8_t previous = 0;
    for (uint16_t mv = 3300; mv <= 4400; mv += 1) {
        const uint8_t p = percent(mv);
        TEST_ASSERT_TRUE_MESSAGE(p >= previous, "percent decreased as voltage rose");
        TEST_ASSERT_TRUE(p <= 100);
        previous = p;
    }
}

// The observed ADC noise is about +/-4 mV. In the flat middle of the curve a
// lot of capacity sits in very little voltage, so a couple of points of swing
// there is physics, not a bug -- which is exactly why the displayed value is
// rounded rather than shown raw.
void test_noise_swing_is_bounded(void) {
    for (uint16_t mv = 3450; mv <= 4340; mv += 10) {
        const int swing = percent(mv + 4) - percent(mv - 4);
        TEST_ASSERT_TRUE_MESSAGE(swing <= 3, "noise moves reading > 3 points");
    }
}

// What the user sees must be a multiple of 5 and never exceed 100.
void test_display_percent_is_rounded(void) {
    for (uint16_t mv = 3300; mv <= 4400; mv += 7) {
        const uint8_t d = battery::display_percent(mv);
        TEST_ASSERT_EQUAL_UINT8(0, d % 5);
        TEST_ASSERT_TRUE(d <= 100);
    }
    TEST_ASSERT_EQUAL_UINT8(100, battery::display_percent(4350));
    TEST_ASSERT_EQUAL_UINT8(0, battery::display_percent(3400));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_endpoints);
    RUN_TEST(test_known_points);
    RUN_TEST(test_interpolates_between_points);
    RUN_TEST(test_monotonic_in_voltage);
    RUN_TEST(test_noise_swing_is_bounded);
    RUN_TEST(test_display_percent_is_rounded);
    return UNITY_END();
}
