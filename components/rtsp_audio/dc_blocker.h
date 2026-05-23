#pragma once

#include <algorithm>
#include <cstdint>

namespace esphome::rtsp_audio::internal {

// One-pole IIR high-pass filter (DC blocker) for 16-bit PCM. Q15 fixed
// point so it runs on ESP32 / S2 (no FPU) at negligible cost.
//
// Difference equation:
//   y[n] = x[n] - x[n-1] + R * y[n-1]
//
// R = exp(-2*pi*fc/fs); at fc = 200 Hz, fs = 16 kHz this is ~0.9245,
// which in Q15 is round(0.9245 * 32768) = 30292.

constexpr int32_t DC_BLOCKER_CUTOFF_HZ = 200;
constexpr int32_t DC_BLOCKER_R_Q15 = 30292;

struct DcBlockerState {
  int32_t x_prev;
  int32_t y_prev;
};

// A large step in x[n] can briefly push y outside the int16 range during
// the transient before R*y decays it back, so the int16 output is
// saturated. The unclamped int32 y is fed back to preserve the filter
// response — clamping the feedback path would warp it on transients.
inline int16_t dc_blocker_step(int16_t x, DcBlockerState &s) {
  const int32_t xi = static_cast<int32_t>(x);
  const int32_t y = xi - s.x_prev + ((DC_BLOCKER_R_Q15 * s.y_prev) >> 15);
  s.x_prev = xi;
  s.y_prev = y;
  return static_cast<int16_t>(std::clamp<int32_t>(y, INT16_MIN, INT16_MAX));
}

}  // namespace esphome::rtsp_audio::internal
