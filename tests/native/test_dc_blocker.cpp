#include <gtest/gtest.h>

#include <cmath>
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

TEST(DcBlocker, FirstSampleIsPassedThrough) {
  // y[0] = x[0] - 0 + 0 = x[0]. Sanity check that state genuinely
  // starts at zero.
  DcBlockerState s{};
  EXPECT_EQ(1234, dc_blocker_step(1234, s));
}

TEST(DcBlocker, DcInputDecaysTowardZero) {
  // The reason this stage exists: kill the MEMS DC offset before it
  // reaches the downstream filters. After enough samples the output
  // should sit very close to 0 even though the input is still a
  // constant 1000. At 5 Hz / 32 kHz the time constant is ~1/(2π·5)
  // ≈ 32 ms ≈ 1000 samples, so 50000 samples (~1.5 s) is many
  // settling times.
  DcBlockerState s{};
  constexpr int16_t kDc = 1000;
  int16_t last = 0;
  for (int i = 0; i < 50000; i++) {
    last = dc_blocker_step(kDc, s);
  }
  // Q15 truncation in the shift leaves a small steady-state residual;
  // the v0.1 100 Hz version held it to ≤ 2 LSB. At 5 Hz the pole
  // sits closer to the unit circle so the residual can be a few LSB
  // larger, but still very small in absolute terms.
  EXPECT_LE(std::abs(static_cast<int>(last)), 10);
}

TEST(DcBlocker, ImpulseDecaysBackToZero) {
  // After an impulse, feed zeros and watch the output decay. The
  // tolerance is generous because Q15 arithmetic-shift quantisation
  // leaves a small DC residual proportional to the impulse magnitude.
  DcBlockerState s{};
  constexpr int16_t kImpulse = 20000;
  dc_blocker_step(kImpulse, s);
  int16_t last = 0;
  for (int i = 0; i < 50000; i++) {
    last = dc_blocker_step(0, s);
  }
  EXPECT_LT(std::abs(static_cast<int>(last)), kImpulse / 100);
}

TEST(DcBlocker, SaturatesOnFullScaleAlternation) {
  // Alternating +full-scale / -full-scale drives the intermediate sum
  // past int16; the returned sample must clamp without wrapping.
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

TEST(DcBlocker, AudioPassesThroughLargelyIntact) {
  // A 1 kHz sine (well above the 5 Hz cutoff) should pass through
  // close to unity gain — confirms the filter isn't accidentally
  // eating mid-band content.
  DcBlockerState s{};
  constexpr double kPi = 3.14159265358979323846;
  constexpr int kCount = 8000;
  int32_t peak_in = 0;
  int32_t peak_out = 0;
  for (int i = 0; i < kCount; i++) {
    const double v = 10000.0 * std::sin(2.0 * kPi * 1000.0 * i / 32000.0);
    const int16_t x = static_cast<int16_t>(std::lround(v));
    const int16_t y = dc_blocker_step(x, s);
    if (i > 200) {  // skip the brief startup transient
      peak_in = std::max<int32_t>(peak_in, std::abs(static_cast<int32_t>(x)));
      peak_out = std::max<int32_t>(peak_out, std::abs(static_cast<int32_t>(y)));
    }
  }
  // At 1 kHz the 5 Hz HP is effectively transparent. Gain is within
  // a few percent of unity — Q15 quantization, the 1-pole shape's
  // mild peak around the cutoff, and peak-of-sine sampling all
  // contribute the small slack.
  const double gain = static_cast<double>(peak_out) / static_cast<double>(peak_in);
  EXPECT_NEAR(1.0, gain, 0.05);
}

TEST(DcBlocker, DefaultsAreFiveHz) {
  // Guard against accidental edits to the fixed coefficient.
  EXPECT_EQ(5, DC_BLOCKER_CUTOFF_HZ);
  EXPECT_EQ(32736, DC_BLOCKER_R_Q15);
}

}  // namespace
}  // namespace esphome::rtsp_audio::internal
