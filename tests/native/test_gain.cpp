#include <gtest/gtest.h>

#include <cstdint>

#include "gain.h"

namespace esphome::rtsp_audio::internal {
namespace {

TEST(GainQ8For, UnityIsExactlyTwoFiftySix) {
  // The pipeline's fast-path skip relies on Q8 == 256 meaning "unity".
  // If the float→Q8 round drifts this even by one, the default install
  // stops being bit-identical to the pre-gain build.
  EXPECT_EQ(GAIN_Q8_UNITY, gain_q8_for(1.0f));
  EXPECT_EQ(256, GAIN_Q8_UNITY);
}

TEST(GainQ8For, KnownPoints) {
  EXPECT_EQ(128, gain_q8_for(0.5f));
  EXPECT_EQ(512, gain_q8_for(2.0f));
  EXPECT_EQ(1024, gain_q8_for(4.0f));
  EXPECT_EQ(4096, gain_q8_for(16.0f));
  EXPECT_EQ(20480, gain_q8_for(80.0f));
}

TEST(GainQ8For, ClampsBelowMin) {
  const int32_t at_min = gain_q8_for(GAIN_MIN);
  EXPECT_EQ(at_min, gain_q8_for(0.0f));
  EXPECT_EQ(at_min, gain_q8_for(-5.0f));
}

TEST(GainQ8For, ClampsAboveMax) {
  const int32_t at_max = gain_q8_for(GAIN_MAX);
  EXPECT_EQ(at_max, gain_q8_for(GAIN_MAX + 10.0f));
  EXPECT_EQ(at_max, gain_q8_for(1e6f));
}

TEST(GainApplyQ8, UnityIsBitIdentical) {
  // Sweep across the int16 range and confirm unity gain returns the
  // input unchanged. This is the fast-path's contract.
  for (int32_t s = INT16_MIN; s <= INT16_MAX; s += 7) {
    const int16_t in = static_cast<int16_t>(s);
    EXPECT_EQ(in, gain_apply_q8(in, GAIN_Q8_UNITY));
  }
}

TEST(GainApplyQ8, MidRangeScaling) {
  // 1000 * 2.0 = 2000; 1000 * 0.5 = 500. Q8 rounding matters at the
  // edges but these midpoints are exact.
  EXPECT_EQ(2000, gain_apply_q8(1000, gain_q8_for(2.0f)));
  EXPECT_EQ(500, gain_apply_q8(1000, gain_q8_for(0.5f)));
  EXPECT_EQ(-2000, gain_apply_q8(-1000, gain_q8_for(2.0f)));
}

TEST(GainApplyQ8, SaturatesOnOverflow) {
  // INT16_MAX * GAIN_MAX vastly exceeds int16. Must clamp, not wrap,
  // in both directions, even at the new 80x ceiling.
  EXPECT_EQ(INT16_MAX, gain_apply_q8(INT16_MAX, gain_q8_for(GAIN_MAX)));
  EXPECT_EQ(INT16_MAX, gain_apply_q8(20000, gain_q8_for(4.0f)));
  EXPECT_EQ(INT16_MAX, gain_apply_q8(500, gain_q8_for(GAIN_MAX)));
  EXPECT_EQ(INT16_MIN, gain_apply_q8(INT16_MIN, gain_q8_for(GAIN_MAX)));
  EXPECT_EQ(INT16_MIN, gain_apply_q8(-20000, gain_q8_for(4.0f)));
  EXPECT_EQ(INT16_MIN, gain_apply_q8(-500, gain_q8_for(GAIN_MAX)));
}

TEST(GainApplyQ8, ZeroInputZeroOutput) {
  // No matter the gain, a zero sample stays a zero sample.
  for (float g = GAIN_MIN; g <= GAIN_MAX; g += 0.37f) {
    EXPECT_EQ(0, gain_apply_q8(0, gain_q8_for(g)));
  }
}

TEST(GainConstants, BoundsAreSane) {
  // Guard the public bounds against accidental edits — the Python
  // schema mirrors these.
  EXPECT_FLOAT_EQ(0.1f, GAIN_MIN);
  EXPECT_FLOAT_EQ(80.0f, GAIN_MAX);
  EXPECT_FLOAT_EQ(1.0f, GAIN_DEFAULT);
}

}  // namespace
}  // namespace esphome::rtsp_audio::internal
