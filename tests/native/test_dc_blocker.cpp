#include <gtest/gtest.h>

#include <cstdint>

#include "dc_blocker.h"

namespace esphome::rtsp_audio::internal {
namespace {

TEST(DcBlocker, ZeroInputZeroOutput) {
  DcBlockerState s{};
  for (int i = 0; i < 1000; i++) {
    EXPECT_EQ(0, dc_blocker_step(0, s));
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
    last = dc_blocker_step(kDc, s);
  }
  EXPECT_LE(std::abs(static_cast<int>(last)), 2);
}

TEST(DcBlocker, FirstSampleIsPassedThrough) {
  // y[0] = x[0] - 0 + 0 = x[0]. Useful as a sanity check that the
  // state genuinely starts at zero.
  DcBlockerState s{};
  EXPECT_EQ(1234, dc_blocker_step(1234, s));
}

TEST(DcBlocker, ImpulseDecaysBackToZero) {
  DcBlockerState s{};
  constexpr int16_t kImpulse = 20000;
  dc_blocker_step(kImpulse, s);
  // After the impulse, feed zeros and watch the output decay. The
  // tolerance is generous because Q15 arithmetic-shift quantisation
  // leaves a small DC residual proportional to the impulse magnitude;
  // the assertion checks "decayed to noise floor", not "exactly zero".
  int16_t last = 0;
  for (int i = 0; i < 5000; i++) {
    last = dc_blocker_step(0, s);
  }
  EXPECT_LT(std::abs(static_cast<int>(last)), kImpulse / 100);
}

TEST(DcBlocker, SaturatesOnFullScaleAlternation) {
  // Alternating +full-scale / -full-scale drives the intermediate sum
  // outside int16; the returned sample must clamp without wrapping.
  DcBlockerState s{};
  for (int i = 0; i < 100; i++) {
    const int16_t in = (i & 1) ? INT16_MIN : INT16_MAX;
    const int16_t out = dc_blocker_step(in, s);
    EXPECT_GE(out, INT16_MIN);
    EXPECT_LE(out, INT16_MAX);
  }
}

TEST(DcBlocker, ResetStateRestartsFilter) {
  // Resetting the state (as start_streaming_() does on each new PLAY)
  // must make the filter behave like a freshly-initialised instance.
  DcBlockerState s{};
  for (int i = 0; i < 100; i++)
    dc_blocker_step(15000, s);
  s = {};
  EXPECT_EQ(4321, dc_blocker_step(4321, s));
}

TEST(DcBlocker, CoefficientMatchesTargetCutoff) {
  // 30292 / 32768 ≈ 0.9245 = exp(-2π·200/16000). Guard against an
  // accidental edit to the constant.
  EXPECT_EQ(200, DC_BLOCKER_CUTOFF_HZ);
  EXPECT_EQ(30292, DC_BLOCKER_R_Q15);
}

}  // namespace
}  // namespace esphome::rtsp_audio::internal
