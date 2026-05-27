#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "biquad.h"

namespace esphome::rtsp_audio::internal {

// 2nd-order Butterworth low-pass (high-cut) for 16-bit PCM. -12 dB/oct
// rolloff, maximally flat passband. Replaces the previous 1-pole
// high-cut — same -3 dB cutoff convention, so any persisted HA slider
// value keeps its meaning, but content above the cutoff is now ~12 dB
// per octave down instead of ~6 dB.
//
// Defaults to the off sentinel (cutoff = 16 kHz, the Nyquist of our
// 32 kHz audio): callers check `high_cut_is_bypass(fc)` and skip the
// stage entirely, so a default install is bit-identical to a build
// without the feature.

constexpr int32_t HIGH_CUT_DEFAULT_CUTOFF_HZ = 16000;
constexpr int32_t HIGH_CUT_MIN_CUTOFF_HZ = 1000;
constexpr int32_t HIGH_CUT_MAX_CUTOFF_HZ = 16000;

// "Filter off" sentinel: at (or above) Nyquist the biquad does no
// useful work and the pipeline short-circuits the stage. Single source
// of truth so the C++ setter and pipeline agree.
inline bool high_cut_is_bypass(float cutoff_hz) {
  return cutoff_hz >= static_cast<float>(HIGH_CUT_MAX_CUTOFF_HZ);
}

// Bilinear transform with prewarping, Q=1/sqrt(2) (Butterworth).
// Caller should check high_cut_is_bypass() first; this still returns
// valid coefficients for any fc < Nyquist. Computed in double for
// clean rounding to float; called only when the cutoff changes
// (rare, HA-driven) so the cost is well off the audio hot path.
inline BiquadCoeffs high_cut_butterworth_coeffs(float cutoff_hz, float sample_rate_hz) {
  const float lo = static_cast<float>(HIGH_CUT_MIN_CUTOFF_HZ);
  const float hi = static_cast<float>(HIGH_CUT_MAX_CUTOFF_HZ);
  const double fc = static_cast<double>(std::clamp(cutoff_hz, lo, hi));
  const double fs = static_cast<double>(sample_rate_hz);

  const double w = std::tan(3.14159265358979323846 * fc / fs);
  const double w2 = w * w;
  const double sqrt2 = 1.41421356237309504880;
  const double norm = 1.0 / (1.0 + sqrt2 * w + w2);

  BiquadCoeffs c{};
  c.b0 = static_cast<float>(w2 * norm);
  c.b1 = static_cast<float>(2.0 * w2 * norm);
  c.b2 = static_cast<float>(w2 * norm);
  c.a1 = static_cast<float>(2.0 * (w2 - 1.0) * norm);
  c.a2 = static_cast<float>((1.0 - sqrt2 * w + w2) * norm);
  return c;
}

}  // namespace esphome::rtsp_audio::internal
