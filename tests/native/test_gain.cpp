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
  EXPECT_EQ(25600, gain_q8_for(100.0f));
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
  EXPECT_FLOAT_EQ(100.0f, GAIN_MAX);
  EXPECT_FLOAT_EQ(1.0f, GAIN_DEFAULT);
  EXPECT_FLOAT_EQ(-20.0f, GAIN_DB_MIN);
  EXPECT_FLOAT_EQ(40.0f, GAIN_DB_MAX);
  EXPECT_FLOAT_EQ(0.0f, GAIN_DB_DEFAULT);
}

TEST(GainDbConversion, ZeroDbIsExactlyUnity) {
  // Bit-identical default depends on 0 dB hitting Q8=256 exactly, not
  // just "close". Any float drift here would silently break the
  // fast-path skip in the audio pipeline.
  EXPECT_EQ(GAIN_Q8_UNITY, gain_q8_for_db(0.0f));
}

TEST(GainDbConversion, KnownPoints) {
  // +20 dB is exactly 10×; check Q8 is at the corresponding linear
  // value (Q8 = round(10 * 256) = 2560).
  EXPECT_EQ(2560, gain_q8_for_db(20.0f));
  // -20 dB is 0.1×; Q8 = round(0.1 * 256) = 26.
  EXPECT_EQ(26, gain_q8_for_db(-20.0f));
  // +40 dB is 100× — the new linear ceiling.
  EXPECT_EQ(gain_q8_for(GAIN_MAX), gain_q8_for_db(40.0f));
  // +6 dB ≈ 1.995× linear → Q8 round to 511; allow ±2 for Q8 rounding.
  EXPECT_NEAR(gain_q8_for(2.0f), gain_q8_for_db(6.0f), 2);
  // -6 dB ≈ 0.501× linear → Q8 ~128; allow ±2.
  EXPECT_NEAR(gain_q8_for(0.5f), gain_q8_for_db(-6.0f), 2);
}

TEST(GainDbConversion, ClampsOutsideRange) {
  // Anything below GAIN_DB_MIN clamps to the GAIN_DB_MIN Q8, same for
  // the upper end. Guards against a hand-rolled `number.set` action
  // bypassing the HA slider bounds.
  const int32_t at_min = gain_q8_for_db(GAIN_DB_MIN);
  EXPECT_EQ(at_min, gain_q8_for_db(-50.0f));
  EXPECT_EQ(at_min, gain_q8_for_db(-9999.0f));

  const int32_t at_max = gain_q8_for_db(GAIN_DB_MAX);
  EXPECT_EQ(at_max, gain_q8_for_db(60.0f));
  EXPECT_EQ(at_max, gain_q8_for_db(9999.0f));
}

TEST(GainDbConversion, MonotonicAcrossRange) {
  // Strictly increasing dB → strictly non-decreasing Q8 (the Q8 round
  // can plateau across very small dB steps near the bounds, so allow
  // equal rather than strictly less).
  int32_t prev = gain_q8_for_db(GAIN_DB_MIN);
  for (float db = GAIN_DB_MIN + 1.0f; db <= GAIN_DB_MAX; db += 1.0f) {
    const int32_t cur = gain_q8_for_db(db);
    EXPECT_GE(cur, prev) << "non-monotonic at db=" << db;
    prev = cur;
  }
}

TEST(LinearToDb, RoundTripsForKnownPoints) {
  // Used only for the dump_config log line — sanity check it agrees
  // with the conversions above.
  EXPECT_NEAR(0.0f, linear_to_db(1.0f), 1e-4f);
  EXPECT_NEAR(20.0f, linear_to_db(10.0f), 1e-4f);
  EXPECT_NEAR(-20.0f, linear_to_db(0.1f), 1e-3f);
  EXPECT_NEAR(6.0f, linear_to_db(db_to_linear(6.0f)), 1e-4f);
}

}  // namespace
}  // namespace esphome::rtsp_audio::internal
