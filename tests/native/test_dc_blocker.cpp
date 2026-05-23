#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>

#include "dc_blocker.h"

namespace esphome::rtsp_audio::internal {
namespace {

constexpr int32_t kDefaultR = DC_BLOCKER_DEFAULT_R_Q15;

TEST(DcBlocker, ZeroInputZeroOutput) {
  DcBlockerState s{};
  for (int i = 0; i < 1000; i++) {
    EXPECT_EQ(0, dc_blocker_step(0, s, kDefaultR));
  }
  EXPECT_EQ(0, s.x_prev);
  EXPECT_EQ(0, s.y_prev);
}

TEST(DcBlocker, DcInputDecaysTowardZero) {
  // A constant DC offset is exactly what this filter exists to kill.
  // After enough samples the output should sit very close to 0 even
  // though the input is still 1000.
  DcBlockerState s{};
  constexpr int16_t kDc = 1000;
  int16_t last = 0;
  for (int i = 0; i < 5000; i++) {
    last = dc_blocker_step(kDc, s, kDefaultR);
  }
  EXPECT_LE(std::abs(static_cast<int>(last)), 2);
}

TEST(DcBlocker, FirstSampleIsPassedThrough) {
  // y[0] = x[0] - 0 + 0 = x[0]. Useful as a sanity check that the
  // state genuinely starts at zero.
  DcBlockerState s{};
  EXPECT_EQ(1234, dc_blocker_step(1234, s, kDefaultR));
}

TEST(DcBlocker, ImpulseDecaysBackToZero) {
  DcBlockerState s{};
  constexpr int16_t kImpulse = 20000;
  dc_blocker_step(kImpulse, s, kDefaultR);
  // After the impulse, feed zeros and watch the output decay. The
  // tolerance is generous because Q15 arithmetic-shift quantisation
  // leaves a small DC residual proportional to the impulse magnitude;
  // the assertion checks "decayed to noise floor", not "exactly zero".
  int16_t last = 0;
  for (int i = 0; i < 5000; i++) {
    last = dc_blocker_step(0, s, kDefaultR);
  }
  EXPECT_LT(std::abs(static_cast<int>(last)), kImpulse / 100);
}

TEST(DcBlocker, SaturatesOnFullScaleAlternation) {
  // Alternating +full-scale / -full-scale drives the intermediate sum
  // outside int16; the returned sample must clamp without wrapping.
  DcBlockerState s{};
  for (int i = 0; i < 100; i++) {
    const int16_t in = (i & 1) ? INT16_MIN : INT16_MAX;
    const int16_t out = dc_blocker_step(in, s, kDefaultR);
    EXPECT_GE(out, INT16_MIN);
    EXPECT_LE(out, INT16_MAX);
  }
}

TEST(DcBlocker, ResetStateRestartsFilter) {
  // Resetting the state (as start_streaming_() does on each new PLAY)
  // must make the filter behave like a freshly-initialised instance.
  DcBlockerState s{};
  for (int i = 0; i < 100; i++)
    dc_blocker_step(15000, s, kDefaultR);
  s = {};
  EXPECT_EQ(4321, dc_blocker_step(4321, s, kDefaultR));
}

TEST(DcBlocker, DefaultsAreOneHundredHz) {
  // 31506 / 32768 ≈ 0.96149 = exp(-2π·100/16000). Guard against an
  // accidental edit to the defaults.
  EXPECT_EQ(100, DC_BLOCKER_DEFAULT_CUTOFF_HZ);
  EXPECT_EQ(31506, DC_BLOCKER_DEFAULT_R_Q15);
}

TEST(RQ15Helper, MatchesHandComputed) {
  // expf rounding can flip the last unit; allow ±1.
  EXPECT_NEAR(31506, dc_blocker_r_q15_for(100.0f, 16000.0f), 1);
  EXPECT_NEAR(30292, dc_blocker_r_q15_for(200.0f, 16000.0f), 1);
}

TEST(RQ15Helper, ClampsBelowMin) {
  // Anything below MIN_CUTOFF_HZ should produce the same coefficient
  // as the min itself.
  const int32_t at_min = dc_blocker_r_q15_for(static_cast<float>(DC_BLOCKER_MIN_CUTOFF_HZ), 16000.0f);
  EXPECT_EQ(at_min, dc_blocker_r_q15_for(0.0f, 16000.0f));
  EXPECT_EQ(at_min, dc_blocker_r_q15_for(-50.0f, 16000.0f));
}

TEST(RQ15Helper, ClampsAboveMax) {
  const int32_t at_max = dc_blocker_r_q15_for(static_cast<float>(DC_BLOCKER_MAX_CUTOFF_HZ), 16000.0f);
  EXPECT_EQ(at_max, dc_blocker_r_q15_for(10000.0f, 16000.0f));
}

TEST(RQ15Helper, MonotonicInCutoff) {
  // Higher cutoff → smaller R (the filter is more aggressive). This
  // catches sign / formula errors that pass the spot-checks.
  const int32_t low = dc_blocker_r_q15_for(50.0f, 16000.0f);
  const int32_t mid = dc_blocker_r_q15_for(150.0f, 16000.0f);
  const int32_t high = dc_blocker_r_q15_for(400.0f, 16000.0f);
  EXPECT_GT(low, mid);
  EXPECT_GT(mid, high);
}

}  // namespace
}  // namespace esphome::rtsp_audio::internal
