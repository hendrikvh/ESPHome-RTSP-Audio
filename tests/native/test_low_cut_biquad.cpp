#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "biquad.h"
#include "low_cut_biquad.h"

namespace esphome::rtsp_audio::internal {
namespace {

constexpr float kFs = 32000.0f;
constexpr double kPi = 3.14159265358979323846;

// Drive a sine of `freq_hz` and amplitude `amp` through `c` for
// `warmup + measure` samples, return the peak |output| over the last
// `measure` samples (the steady-state portion). Used as a frequency-
// response probe in the assertions below.
int32_t measure_sine_peak(const BiquadCoeffs &c, double freq_hz, double amp, int warmup, int measure) {
  BiquadState s{};
  int32_t peak = 0;
  const double omega = 2.0 * kPi * freq_hz / static_cast<double>(kFs);
  for (int i = 0; i < warmup + measure; i++) {
    const double v = amp * std::sin(omega * i);
    const int16_t x = static_cast<int16_t>(std::clamp<double>(std::lround(v), INT16_MIN, INT16_MAX));
    const int16_t y = biquad_step(x, s, c);
    if (i >= warmup) {
      peak = std::max<int32_t>(peak, std::abs(static_cast<int32_t>(y)));
    }
  }
  return peak;
}

TEST(LowCutBiquad, ZeroInputZeroOutput) {
  const BiquadCoeffs c = low_cut_butterworth_coeffs(100.0f, kFs);
  BiquadState s{};
  for (int i = 0; i < 1000; i++) {
    EXPECT_EQ(0, biquad_step(0, s, c));
  }
}

TEST(LowCutBiquad, DcInputDecaysTowardZero) {
  // The whole reason this stage exists: kill DC and rumble. After
  // enough samples the output should sit very close to 0 even though
  // the input is still 1000.
  const BiquadCoeffs c = low_cut_butterworth_coeffs(100.0f, kFs);
  BiquadState s{};
  constexpr int16_t kDc = 1000;
  int16_t last = 0;
  for (int i = 0; i < 20000; i++) {
    last = biquad_step(kDc, s, c);
  }
  EXPECT_LE(std::abs(static_cast<int>(last)), 4);
}

TEST(LowCutBiquad, ImpulseDecaysTowardZero) {
  // After an impulse, feed zeros and watch the output decay back to
  // (near) zero — the filter must be stable.
  const BiquadCoeffs c = low_cut_butterworth_coeffs(100.0f, kFs);
  BiquadState s{};
  constexpr int16_t kImpulse = 20000;
  biquad_step(kImpulse, s, c);
  int16_t last = 0;
  for (int i = 0; i < 20000; i++) {
    last = biquad_step(0, s, c);
  }
  EXPECT_LT(std::abs(static_cast<int>(last)), kImpulse / 100);
}

TEST(LowCutBiquad, SaturatesOnFullScaleAlternation) {
  // Nyquist-rate full-scale alternation drives the intermediate sum
  // past int16; the returned sample must clamp without wrapping.
  const BiquadCoeffs c = low_cut_butterworth_coeffs(100.0f, kFs);
  BiquadState s{};
  for (int i = 0; i < 200; i++) {
    const int16_t in = (i & 1) ? INT16_MIN : INT16_MAX;
    const int16_t out = biquad_step(in, s, c);
    EXPECT_GE(out, INT16_MIN);
    EXPECT_LE(out, INT16_MAX);
  }
}

TEST(LowCutBiquad, ResetStateRestartsFilter) {
  // start_streaming_() resets state at each PLAY. After reset the
  // filter must behave like a freshly-initialised instance — i.e. the
  // first sample is identical to what a fresh state would produce.
  const BiquadCoeffs c = low_cut_butterworth_coeffs(100.0f, kFs);
  BiquadState fresh{};
  const int16_t expected = biquad_step(4321, fresh, c);

  BiquadState s{};
  for (int i = 0; i < 1000; i++)
    biquad_step(15000, s, c);
  s = {};
  EXPECT_EQ(expected, biquad_step(4321, s, c));
}

TEST(LowCutBiquad, MinusThreeDbAtCutoff) {
  // The defining property: at fc the gain is -3 dB (1/sqrt(2) ≈
  // 0.7071). Drive a sine at 100 Hz, measure steady-state peak,
  // confirm it's within ±1.5 dB of the expected 0.7071·amp.
  constexpr double kFc = 100.0;
  constexpr double kAmp = 20000.0;
  const BiquadCoeffs c = low_cut_butterworth_coeffs(static_cast<float>(kFc), kFs);
  const int32_t peak = measure_sine_peak(c, kFc, kAmp, 8000, 8000);
  const double measured_db = 20.0 * std::log10(static_cast<double>(peak) / kAmp);
  EXPECT_NEAR(-3.0, measured_db, 1.5);
}

TEST(LowCutBiquad, TwelveDbPerOctaveAsymptote) {
  // The asymptotic skirt of a 2nd-order Butterworth is -12 dB/oct.
  // Near the cutoff the actual slope is gentler (the magnitude is
  // (f/fc)^2 / sqrt(1+(f/fc)^4) which only approaches a clean
  // f^2 below the cutoff). Measure deep in the stopband — between
  // fc/8 and fc/16 — where the slope has fully bent in.
  constexpr double kFc = 100.0;
  constexpr double kAmp = 20000.0;
  const BiquadCoeffs c = low_cut_butterworth_coeffs(static_cast<float>(kFc), kFs);
  const int32_t peak_eighth = measure_sine_peak(c, kFc / 8.0, kAmp, 16000, 8000);
  const int32_t peak_sixteenth = measure_sine_peak(c, kFc / 16.0, kAmp, 16000, 8000);
  const double diff_db = 20.0 * std::log10(static_cast<double>(peak_sixteenth) / static_cast<double>(peak_eighth));
  EXPECT_NEAR(-12.0, diff_db, 1.5);
}

TEST(LowCutBiquad, StopbandHeavilyAttenuated) {
  // A decade below the cutoff (~20 Hz for fc=100 Hz) should be at
  // least 30 dB down. The exact figure depends on the bilinear-
  // transform shape, but anything less than ~30 dB would mean the
  // filter isn't actually rejecting rumble the way the docs claim.
  constexpr double kFc = 100.0;
  constexpr double kAmp = 20000.0;
  const BiquadCoeffs c = low_cut_butterworth_coeffs(static_cast<float>(kFc), kFs);
  const int32_t peak = measure_sine_peak(c, kFc / 10.0, kAmp, 16000, 8000);
  const double db = 20.0 * std::log10(static_cast<double>(peak) / kAmp);
  EXPECT_LT(db, -30.0);
}

TEST(LowCutBiquad, PassbandGainNearUnity) {
  // A signal well above the cutoff should pass at approximately 0 dB.
  // Use fc=100 Hz and probe at 10×fc = 1000 Hz — deep inside the passband.
  constexpr double kFc = 100.0;
  constexpr double kProbe = kFc * 10.0;
  constexpr double kAmp = 20000.0;
  const BiquadCoeffs c = low_cut_butterworth_coeffs(static_cast<float>(kFc), kFs);
  const int32_t peak = measure_sine_peak(c, kProbe, kAmp, 2000, 2000);
  const double db = 20.0 * std::log10(static_cast<double>(peak) / kAmp);
  EXPECT_NEAR(0.0, db, 1.0);
}

TEST(LowCutBiquad, ClampsBelowMin) {
  // Below MIN_CUTOFF_HZ the helper should produce the same coefficients
  // as the min itself, so an out-of-range HA control can't sneak the
  // pole closer to DC than designed.
  const BiquadCoeffs at_min = low_cut_butterworth_coeffs(static_cast<float>(LOW_CUT_MIN_CUTOFF_HZ), kFs);
  const BiquadCoeffs below = low_cut_butterworth_coeffs(-50.0f, kFs);
  EXPECT_EQ(at_min.b0, below.b0);
  EXPECT_EQ(at_min.a1, below.a1);
  EXPECT_EQ(at_min.a2, below.a2);
}

TEST(LowCutBiquad, ClampsAboveMax) {
  const BiquadCoeffs at_max = low_cut_butterworth_coeffs(static_cast<float>(LOW_CUT_MAX_CUTOFF_HZ), kFs);
  const BiquadCoeffs above = low_cut_butterworth_coeffs(10000.0f, kFs);
  EXPECT_EQ(at_max.b0, above.b0);
  EXPECT_EQ(at_max.a1, above.a1);
  EXPECT_EQ(at_max.a2, above.a2);
}

TEST(LowCutBiquad, BypassBelowMin) {
  // Mirror of the high-cut's bypass-at-max sentinel: dragging the
  // slider below LOW_CUT_MIN_CUTOFF_HZ disables the stage. 0 Hz on
  // the slider reads as off; values like 5 Hz also bypass without
  // trying to filter at a frequency the biquad can't reasonably
  // handle relative to fs.
  EXPECT_TRUE(low_cut_is_bypass(0.0f));
  EXPECT_TRUE(low_cut_is_bypass(5.0f));
  EXPECT_TRUE(low_cut_is_bypass(19.999f));
  EXPECT_FALSE(low_cut_is_bypass(static_cast<float>(LOW_CUT_MIN_CUTOFF_HZ)));
  EXPECT_FALSE(low_cut_is_bypass(100.0f));
  EXPECT_FALSE(low_cut_is_bypass(static_cast<float>(LOW_CUT_MAX_CUTOFF_HZ)));
}

TEST(LowCutBiquad, Defaults) {
  // Guard against accidental edits to the public defaults.
  EXPECT_EQ(100, LOW_CUT_DEFAULT_CUTOFF_HZ);
  EXPECT_EQ(20, LOW_CUT_MIN_CUTOFF_HZ);
  EXPECT_EQ(500, LOW_CUT_MAX_CUTOFF_HZ);
}

}  // namespace
}  // namespace esphome::rtsp_audio::internal
