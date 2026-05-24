#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>

#include "high_cut.h"

namespace esphome::rtsp_audio::internal {
namespace {

// A representative on-coefficient: 2 kHz cutoff at the standard 32 kHz
// rate. Picked so the LPF tests have a clearly audible roll-off without
// being so aggressive that quantisation dominates.
constexpr int32_t kOnA = []() {
  // expf rounding can flip the last unit but a constexpr lambda can't
  // call std::exp on every compiler, so just precompute by hand:
  // a = 1 - exp(-2π·2000/32000) = 1 - exp(-0.3927) ≈ 0.32477
  // a_q15 = round(0.32477 * 32768) = 10642.
  return 10642;
}();

TEST(HighCut, ZeroInputZeroOutput) {
  HighCutState s{};
  for (int i = 0; i < 1000; i++) {
    EXPECT_EQ(0, high_cut_step(0, s, kOnA));
  }
  EXPECT_EQ(0, s.y_prev);
}

TEST(HighCut, ConstantInputSettlesToInput) {
  // An LPF passes DC: the output should converge to the input.
  HighCutState s{};
  constexpr int16_t kDc = 1000;
  int16_t last = 0;
  for (int i = 0; i < 5000; i++) {
    last = high_cut_step(kDc, s, kOnA);
  }
  // Q15 truncation in the shift leaves a small steady-state offset that
  // scales as ~1/a; at a≈0.325 (2 kHz @ 32 kHz) the residual is a few LSBs.
  EXPECT_LE(std::abs(static_cast<int>(last) - kDc), 4);
}

TEST(HighCut, FirstSampleScaledByA) {
  // y[0] = 0 + a*(x[0] - 0) = a*x[0]. For kOnA ≈ 0.325 and x = 1000,
  // y[0] ≈ 325. Catches a sign / formula error that would still pass
  // ZeroInputZeroOutput.
  HighCutState s{};
  const int16_t got = high_cut_step(1000, s, kOnA);
  const int32_t expected = (kOnA * 1000) >> 15;
  EXPECT_EQ(expected, static_cast<int32_t>(got));
}

TEST(HighCut, ImpulseDecaysTowardZero) {
  HighCutState s{};
  constexpr int16_t kImpulse = 20000;
  high_cut_step(kImpulse, s, kOnA);
  int16_t last = 0;
  for (int i = 0; i < 5000; i++) {
    last = high_cut_step(0, s, kOnA);
  }
  EXPECT_LT(std::abs(static_cast<int>(last)), kImpulse / 100);
}

TEST(HighCut, LpfAttenuatesHighFrequency) {
  // Nyquist-rate square wave (alternating ±A) is the worst case for an
  // LPF — steady-state output magnitude should be far below A.
  HighCutState s{};
  constexpr int16_t kAmp = 10000;
  int32_t max_abs = 0;
  // Skip the first few samples so we measure steady state, not the
  // ramp-up transient.
  for (int i = 0; i < 500; i++) {
    const int16_t in = (i & 1) ? -kAmp : kAmp;
    const int16_t out = high_cut_step(in, s, kOnA);
    if (i > 100) {
      max_abs = std::max<int32_t>(max_abs, std::abs(static_cast<int32_t>(out)));
    }
  }
  EXPECT_LT(max_abs, kAmp / 2);
}

TEST(HighCut, SaturatesOnFullScaleAlternation) {
  HighCutState s{};
  for (int i = 0; i < 100; i++) {
    const int16_t in = (i & 1) ? INT16_MIN : INT16_MAX;
    const int16_t out = high_cut_step(in, s, kOnA);
    EXPECT_GE(out, INT16_MIN);
    EXPECT_LE(out, INT16_MAX);
  }
}

TEST(HighCut, ResetStateRestartsFilter) {
  HighCutState s{};
  for (int i = 0; i < 100; i++)
    high_cut_step(15000, s, kOnA);
  s = {};
  // After reset, first sample is a*x as in FirstSampleScaledByA.
  const int16_t got = high_cut_step(4321, s, kOnA);
  const int32_t expected = (kOnA * 4321) >> 15;
  EXPECT_EQ(expected, static_cast<int32_t>(got));
}

TEST(HighCut, DefaultsAreOff) {
  EXPECT_EQ(16000, HIGH_CUT_DEFAULT_CUTOFF_HZ);
  EXPECT_EQ(16000, HIGH_CUT_MAX_CUTOFF_HZ);
  EXPECT_EQ(1000, HIGH_CUT_MIN_CUTOFF_HZ);
  EXPECT_EQ(HIGH_CUT_A_Q15_OFF, HIGH_CUT_DEFAULT_A_Q15);
  EXPECT_EQ(0, HIGH_CUT_A_Q15_OFF);
}

TEST(HighCutAQ15Helper, MatchesHandComputed) {
  // a = 1 - exp(-2π·2000/32000) ≈ 0.324766 → q15 ≈ 10642. expf rounding
  // can flip the last unit; allow ±1.
  EXPECT_NEAR(10642, high_cut_a_q15_for(2000.0f, 32000.0f), 1);
  // a = 1 - exp(-2π·5000/32000) ≈ 0.625344 → q15 ≈ 20491.
  EXPECT_NEAR(20491, high_cut_a_q15_for(5000.0f, 32000.0f), 1);
}

TEST(HighCutAQ15Helper, ClampsBelowMinReturnsAtMin) {
  const int32_t at_min = high_cut_a_q15_for(static_cast<float>(HIGH_CUT_MIN_CUTOFF_HZ), 32000.0f);
  EXPECT_EQ(at_min, high_cut_a_q15_for(0.0f, 32000.0f));
  EXPECT_EQ(at_min, high_cut_a_q15_for(-50.0f, 32000.0f));
  EXPECT_EQ(at_min, high_cut_a_q15_for(500.0f, 32000.0f));
}

TEST(HighCutAQ15Helper, ClampsAboveMaxReturnsOff) {
  EXPECT_EQ(HIGH_CUT_A_Q15_OFF, high_cut_a_q15_for(static_cast<float>(HIGH_CUT_MAX_CUTOFF_HZ), 32000.0f));
  EXPECT_EQ(HIGH_CUT_A_Q15_OFF, high_cut_a_q15_for(30000.0f, 32000.0f));
  EXPECT_EQ(HIGH_CUT_A_Q15_OFF, high_cut_a_q15_for(1.0e9f, 32000.0f));
}

TEST(HighCutAQ15Helper, MonotonicInCutoff) {
  // Higher cutoff → larger a (the filter passes more) — strictly true
  // until we hit the off sentinel. Excludes the cutoff frequencies that
  // could land on the sentinel.
  const int32_t low = high_cut_a_q15_for(1500.0f, 32000.0f);
  const int32_t mid = high_cut_a_q15_for(3000.0f, 32000.0f);
  const int32_t high = high_cut_a_q15_for(6000.0f, 32000.0f);
  EXPECT_LT(low, mid);
  EXPECT_LT(mid, high);
}

}  // namespace
}  // namespace esphome::rtsp_audio::internal
