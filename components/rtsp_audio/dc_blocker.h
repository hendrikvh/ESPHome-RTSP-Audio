#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace esphome::rtsp_audio::internal {

// One-pole IIR high-pass filter (DC blocker) for 16-bit PCM. Q15 fixed
// point so the inner per-sample step runs on ESP32 / S2 (no FPU) at
// negligible cost; the Q15 coefficient itself is recomputed in float
// only when the cutoff changes (rare, HA-driven), well off the audio
// hot path.
//
// Difference equation:
//   y[n] = x[n] - x[n-1] + R * y[n-1]
//
// R = exp(-2*pi*fc/fs). At fc = 100 Hz, fs = 16 kHz this is ~0.96149,
// which in Q15 is round(0.96149 * 32768) = 31506. 100 Hz sits below
// typical voice fundamentals so the default removes HVAC/handling
// rumble without eating into vocal warmth.

constexpr int32_t DC_BLOCKER_DEFAULT_CUTOFF_HZ = 100;
constexpr int32_t DC_BLOCKER_DEFAULT_R_Q15 = 31506;
constexpr int32_t DC_BLOCKER_MIN_CUTOFF_HZ = 20;
constexpr int32_t DC_BLOCKER_MAX_CUTOFF_HZ = 500;

struct DcBlockerState {
  int32_t x_prev;
  int32_t y_prev;
};

// A large step in x[n] can briefly push y outside the int16 range during
// the transient before R*y decays it back, so the int16 output is
// saturated. The unclamped int32 y is fed back to preserve the filter
// response — clamping the feedback path would warp it on transients.
inline int16_t dc_blocker_step(int16_t x, DcBlockerState &s, int32_t r_q15) {
  const int32_t xi = static_cast<int32_t>(x);
  const int32_t y = xi - s.x_prev + ((r_q15 * s.y_prev) >> 15);
  s.x_prev = xi;
  s.y_prev = y;
  return static_cast<int16_t>(std::clamp<int32_t>(y, INT16_MIN, INT16_MAX));
}

// Computes the Q15 coefficient for a given cutoff at a given sample
// rate. Cutoff is clamped to [DC_BLOCKER_MIN_CUTOFF_HZ,
// DC_BLOCKER_MAX_CUTOFF_HZ] so an out-of-range HA control can't
// destabilise the filter.
inline int32_t dc_blocker_r_q15_for(float cutoff_hz, float sample_rate_hz) {
  const float lo = static_cast<float>(DC_BLOCKER_MIN_CUTOFF_HZ);
  const float hi = static_cast<float>(DC_BLOCKER_MAX_CUTOFF_HZ);
  const float fc = std::clamp(cutoff_hz, lo, hi);
  const float r = std::exp(-2.0f * 3.14159265358979323846f * fc / sample_rate_hz);
  return static_cast<int32_t>(std::lround(r * 32768.0f));
}

}  // namespace esphome::rtsp_audio::internal
