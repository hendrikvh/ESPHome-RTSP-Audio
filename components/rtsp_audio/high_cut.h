#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace esphome::rtsp_audio::internal {

// One-pole IIR low-pass filter (high-cut) for 16-bit PCM. Same Q15 shape
// as the DC blocker so the inner per-sample step is a single multiply +
// arithmetic shift on ESP32 / S2 (no FPU). The Q15 coefficient is
// recomputed in float only when the cutoff changes — well off the audio
// hot path.
//
// Difference equation:
//   y[n] = y[n-1] + a * (x[n] - y[n-1])
//   a    = 1 - exp(-2*pi*fc/fs)
//
// Defaults to off (cutoff = 16 kHz, at Nyquist for our 32 kHz audio).
// `HIGH_CUT_A_Q15_OFF` (0) is reserved as a sentinel: the audio pipeline
// checks for it and skips the stage entirely, so the default install is
// bit-identical to a build without the high-cut feature.

constexpr int32_t HIGH_CUT_DEFAULT_CUTOFF_HZ = 16000;
constexpr int32_t HIGH_CUT_MIN_CUTOFF_HZ = 1000;
constexpr int32_t HIGH_CUT_MAX_CUTOFF_HZ = 16000;
constexpr int32_t HIGH_CUT_A_Q15_OFF = 0;
constexpr int32_t HIGH_CUT_DEFAULT_A_Q15 = HIGH_CUT_A_Q15_OFF;

struct HighCutState {
  int32_t y_prev;
};

// On a large step in x[n] the intermediate y can briefly exceed the int16
// range; the int16 output is saturated. The unclamped int32 y is fed
// back so the filter response stays intact on transients — clamping the
// feedback path would warp it.
inline int16_t high_cut_step(int16_t x, HighCutState &s, int32_t a_q15) {
  const int32_t xi = static_cast<int32_t>(x);
  const int32_t y = s.y_prev + ((a_q15 * (xi - s.y_prev)) >> 15);
  s.y_prev = y;
  return static_cast<int16_t>(std::clamp<int32_t>(y, INT16_MIN, INT16_MAX));
}

// Computes the Q15 coefficient for a given cutoff at a given sample
// rate. Cutoff is clamped to [HIGH_CUT_MIN_CUTOFF_HZ,
// HIGH_CUT_MAX_CUTOFF_HZ] so an out-of-range HA control can't
// destabilise the filter. Returns HIGH_CUT_A_Q15_OFF when the cutoff
// hits the max (16 kHz) so the pipeline can short-circuit the stage.
inline int32_t high_cut_a_q15_for(float cutoff_hz, float sample_rate_hz) {
  const float lo = static_cast<float>(HIGH_CUT_MIN_CUTOFF_HZ);
  const float hi = static_cast<float>(HIGH_CUT_MAX_CUTOFF_HZ);
  const float fc = std::clamp(cutoff_hz, lo, hi);
  if (fc >= hi)
    return HIGH_CUT_A_Q15_OFF;
  const float a = 1.0f - std::exp(-2.0f * 3.14159265358979323846f * fc / sample_rate_hz);
  return static_cast<int32_t>(std::lround(a * 32768.0f));
}

}  // namespace esphome::rtsp_audio::internal
