#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "biquad.h"
#include "high_cut_biquad.h"

namespace esphome::rtsp_audio::internal {
namespace {

constexpr float kFs = 32000.0f;
constexpr double kPi = 3.14159265358979323846;

// Drive a sine through `c` and return the steady-state peak |output|.
// Same shape as the low-cut version; duplicated here so each test file
// stays self-contained.
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

TEST(HighCutBiquad, ZeroInputZeroOutput) {
  const BiquadCoeffs c = high_cut_butterworth_coeffs(10000.0f, kFs);
  BiquadState s{};
  for (int i = 0; i < 1000; i++) {
    EXPECT_EQ(0, biquad_step(0, s, c));
  }
}

TEST(HighCutBiquad, ConstantInputSettlesToInput) {
  // An LPF passes DC: the output should converge to the input.
  const BiquadCoeffs c = high_cut_butterworth_coeffs(2000.0f, kFs);
  BiquadState s{};
  constexpr int16_t kDc = 1000;
  int16_t last = 0;
  for (int i = 0; i < 5000; i++) {
    last = biquad_step(kDc, s, c);
  }
  EXPECT_LE(std::abs(static_cast<int>(last) - kDc), 4);
}

TEST(HighCutBiquad, ImpulseDecaysTowardZero) {
  const BiquadCoeffs c = high_cut_butterworth_coeffs(2000.0f, kFs);
  BiquadState s{};
  constexpr int16_t kImpulse = 20000;
  biquad_step(kImpulse, s, c);
  int16_t last = 0;
  for (int i = 0; i < 5000; i++) {
    last = biquad_step(0, s, c);
  }
  EXPECT_LT(std::abs(static_cast<int>(last)), kImpulse / 100);
}

TEST(HighCutBiquad, SaturatesOnFullScaleAlternation) {
  const BiquadCoeffs c = high_cut_butterworth_coeffs(2000.0f, kFs);
  BiquadState s{};
  for (int i = 0; i < 200; i++) {
    const int16_t in = (i & 1) ? INT16_MIN : INT16_MAX;
    const int16_t out = biquad_step(in, s, c);
    EXPECT_GE(out, INT16_MIN);
    EXPECT_LE(out, INT16_MAX);
  }
}

TEST(HighCutBiquad, MinusThreeDbAtCutoff) {
  // At the cutoff the gain should be -3 dB (1/sqrt(2) ≈ 0.7071). Use a
  // 10 kHz cutoff — high enough that the bilinear-transform warping
  // pulls the actual -3 dB point a bit lower in frequency, so the
  // tolerance is generous (within ±2 dB of -3 dB at f = fc itself).
  constexpr double kFc = 10000.0;
  constexpr double kAmp = 20000.0;
  const BiquadCoeffs c = high_cut_butterworth_coeffs(static_cast<float>(kFc), kFs);
  const int32_t peak = measure_sine_peak(c, kFc, kAmp, 2000, 2000);
  const double measured_db = 20.0 * std::log10(static_cast<double>(peak) / kAmp);
  EXPECT_NEAR(-3.0, measured_db, 2.0);
}

TEST(HighCutBiquad, MinusThreeDbAtCutoffLowFc) {
  // The bilinear-transform warping is smaller when fc << fs/2, so this
  // is the cleanest place to pin the -3 dB-at-cutoff invariant.
  constexpr double kFc = 2000.0;
  constexpr double kAmp = 20000.0;
  const BiquadCoeffs c = high_cut_butterworth_coeffs(static_cast<float>(kFc), kFs);
  const int32_t peak = measure_sine_peak(c, kFc, kAmp, 2000, 2000);
  const double measured_db = 20.0 * std::log10(static_cast<double>(peak) / kAmp);
  EXPECT_NEAR(-3.0, measured_db, 1.0);
}

TEST(HighCutBiquad, TwelveDbPerOctaveAboveCutoff) {
  // One octave above the cutoff should be ~12 dB further down than at
  // the cutoff. Use fc = 2 kHz so 2·fc (4 kHz) is well below Nyquist
  // and the slope hasn't started bending in.
  constexpr double kFc = 2000.0;
  constexpr double kAmp = 20000.0;
  const BiquadCoeffs c = high_cut_butterworth_coeffs(static_cast<float>(kFc), kFs);
  const int32_t peak_fc = measure_sine_peak(c, kFc, kAmp, 2000, 2000);
  const int32_t peak_2fc = measure_sine_peak(c, 2.0 * kFc, kAmp, 2000, 2000);
  const double diff_db = 20.0 * std::log10(static_cast<double>(peak_2fc) / static_cast<double>(peak_fc));
  EXPECT_NEAR(-12.0, diff_db, 2.5);
}

TEST(HighCutBiquad, StopbandHeavilyAttenuated) {
  // 5× the cutoff (10 kHz for fc=2 kHz) — comfortably below Nyquist —
  // should be at least 30 dB below the passband.
  constexpr double kFc = 2000.0;
  constexpr double kAmp = 20000.0;
  const BiquadCoeffs c = high_cut_butterworth_coeffs(static_cast<float>(kFc), kFs);
  const int32_t peak = measure_sine_peak(c, 5.0 * kFc, kAmp, 2000, 4000);
  const double db = 20.0 * std::log10(static_cast<double>(peak) / kAmp);
  EXPECT_LT(db, -30.0);
}

TEST(HighCutBiquad, BypassAtNyquist) {
  // Default (16 kHz, Nyquist for 32 kHz audio) reports bypass; the
  // pipeline short-circuits the stage so an un-touched HA install
  // streams bit-identical bytes to a build without the feature.
  EXPECT_TRUE(high_cut_is_bypass(static_cast<float>(HIGH_CUT_MAX_CUTOFF_HZ)));
  EXPECT_TRUE(high_cut_is_bypass(20000.0f));
  EXPECT_FALSE(high_cut_is_bypass(10000.0f));
  EXPECT_FALSE(high_cut_is_bypass(static_cast<float>(HIGH_CUT_MIN_CUTOFF_HZ)));
}

TEST(HighCutBiquad, ResetStateRestartsFilter) {
  // start_streaming_() zeros BiquadState on each PLAY. After reset the
  // filter must behave like a freshly-initialised instance — the first
  // output must be identical to what a fresh state produces for the same
  // input sample.
  const BiquadCoeffs c = high_cut_butterworth_coeffs(2000.0f, kFs);
  BiquadState fresh{};
  const int16_t expected = biquad_step(4321, fresh, c);

  BiquadState s{};
  for (int i = 0; i < 1000; i++)
    biquad_step(15000, s, c);
  s = {};
  EXPECT_EQ(expected, biquad_step(4321, s, c));
}

TEST(HighCutBiquad, PassbandGainNearUnity) {
  // A signal well below the cutoff should pass at approximately 0 dB.
  // Use fc=2 kHz and probe at fc/10 = 200 Hz — deep inside the passband.
  constexpr double kFc = 2000.0;
  constexpr double kProbe = kFc / 10.0;
  constexpr double kAmp = 20000.0;
  const BiquadCoeffs c = high_cut_butterworth_coeffs(static_cast<float>(kFc), kFs);
  const int32_t peak = measure_sine_peak(c, kProbe, kAmp, 2000, 2000);
  const double db = 20.0 * std::log10(static_cast<double>(peak) / kAmp);
  EXPECT_NEAR(0.0, db, 1.0);
}

TEST(HighCutBiquad, ClampsBelowMin) {
  const BiquadCoeffs at_min = high_cut_butterworth_coeffs(static_cast<float>(HIGH_CUT_MIN_CUTOFF_HZ), kFs);
  const BiquadCoeffs below = high_cut_butterworth_coeffs(500.0f, kFs);
  EXPECT_EQ(at_min.b0, below.b0);
  EXPECT_EQ(at_min.a1, below.a1);
  EXPECT_EQ(at_min.a2, below.a2);
}

TEST(HighCutBiquad, Defaults) {
  EXPECT_EQ(16000, HIGH_CUT_DEFAULT_CUTOFF_HZ);
  EXPECT_EQ(1000, HIGH_CUT_MIN_CUTOFF_HZ);
  EXPECT_EQ(16000, HIGH_CUT_MAX_CUTOFF_HZ);
  EXPECT_TRUE(high_cut_is_bypass(static_cast<float>(HIGH_CUT_DEFAULT_CUTOFF_HZ)));
}

}  // namespace
}  // namespace esphome::rtsp_audio::internal
