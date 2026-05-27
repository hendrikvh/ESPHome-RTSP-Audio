#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "biquad.h"

namespace esphome::rtsp_audio::internal {

// 2nd-order Butterworth high-pass (low-cut) for 16-bit PCM. -12 dB/oct
// rolloff, maximally flat passband. Replaces the previous 1-pole low-cut
// stage — same -3 dB cutoff convention, so any persisted HA slider value
// keeps its meaning, but rumble at half the cutoff is now ~12 dB down
// instead of ~6 dB.
//
// 100 Hz default sits below typical voice fundamentals so it removes
// HVAC and handling rumble without eating into vocal warmth.

constexpr int32_t LOW_CUT_DEFAULT_CUTOFF_HZ = 100;
constexpr int32_t LOW_CUT_MIN_CUTOFF_HZ = 20;
constexpr int32_t LOW_CUT_MAX_CUTOFF_HZ = 500;

// "Filter off" sentinel: dragging the slider below the lowest active
// cutoff disables the stage entirely. Mirrors how the high-cut treats
// the top of its range as off — same convention at both ends. The
// upstream always-on DC blocker keeps running either way, so the MEMS
// DC offset never leaks downstream.
inline bool low_cut_is_bypass(float cutoff_hz) {
  return cutoff_hz < static_cast<float>(LOW_CUT_MIN_CUTOFF_HZ);
}

// Bilinear transform with prewarping, Q=1/sqrt(2) (Butterworth).
// Returns the float coefficients ready for biquad_step. Cutoff is
// clamped to [LOW_CUT_MIN_CUTOFF_HZ, LOW_CUT_MAX_CUTOFF_HZ] so an
// out-of-range HA control can't destabilise the filter. Computed in
// double internally for clean rounding to float; called only when the
// cutoff changes (rare, HA-driven) so the cost is well off the audio
// hot path.
inline BiquadCoeffs low_cut_butterworth_coeffs(float cutoff_hz, float sample_rate_hz) {
  const float lo = static_cast<float>(LOW_CUT_MIN_CUTOFF_HZ);
  const float hi = static_cast<float>(LOW_CUT_MAX_CUTOFF_HZ);
  const double fc = static_cast<double>(std::clamp(cutoff_hz, lo, hi));
  const double fs = static_cast<double>(sample_rate_hz);

  const double w = std::tan(3.14159265358979323846 * fc / fs);
  const double w2 = w * w;
  const double sqrt2 = 1.41421356237309504880;
  const double norm = 1.0 / (1.0 + sqrt2 * w + w2);

  BiquadCoeffs c{};
  c.b0 = static_cast<float>(norm);
  c.b1 = static_cast<float>(-2.0 * norm);
  c.b2 = static_cast<float>(norm);
  c.a1 = static_cast<float>(2.0 * (w2 - 1.0) * norm);
  c.a2 = static_cast<float>((1.0 - sqrt2 * w + w2) * norm);
  return c;
}

}  // namespace esphome::rtsp_audio::internal
