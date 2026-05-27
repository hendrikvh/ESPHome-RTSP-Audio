#pragma once

#include <algorithm>
#include <cstdint>

namespace esphome::rtsp_audio::internal {

// Always-on, fixed-frequency 1-pole IIR high-pass that strips the DC
// offset MEMS capsules (e.g. INMP441) ship with, before any other DSP
// stage sees the audio. Not user-configurable — its job is hygiene,
// not tone shaping; the user-tunable Butterworth low-cut sits
// downstream for the audible roll-off.
//
// Difference equation:
//   y[n] = x[n] - x[n-1] + R * y[n-1]
//   R    = exp(-2*pi*fc/fs)  ;  fc = 5 Hz, fs = 32 kHz
//
// The `x − x_prev` term is a true differentiator on the input — DC
// converges in a single pass, not as a fixed-point of the recurrence
// — so this 1-pole filter doesn't suffer the limit-cycle dead-band
// that drove the user-facing 2nd-order biquad to float. Q15 fixed-
// point, one MAC per sample, runs at the same cost on every supported
// ESP target.
//
// R = exp(-2π·5/32000) ≈ 0.99902. In Q15 that's
// round(0.99902 * 32768) = 32736. Hard-coded — recomputing at runtime
// adds nothing because fs is fixed at 32 kHz and fc is fixed at 5 Hz.

constexpr int32_t DC_BLOCKER_CUTOFF_HZ = 5;
constexpr int32_t DC_BLOCKER_R_Q15 = 32736;

struct DcBlockerState {
  int32_t x_prev;
  int32_t y_prev;
};

// Per-sample step. Saturation on the int16 output mirrors the
// biquads: the unclamped int32 y is fed back so a large impulse
// doesn't warp the filter response on the way out.
inline int16_t dc_blocker_step(int16_t x, DcBlockerState &s) {
  const int32_t xi = static_cast<int32_t>(x);
  const int32_t y = xi - s.x_prev + ((DC_BLOCKER_R_Q15 * s.y_prev) >> 15);
  s.x_prev = xi;
  s.y_prev = y;
  return static_cast<int16_t>(std::clamp<int32_t>(y, INT16_MIN, INT16_MAX));
}

}  // namespace esphome::rtsp_audio::internal
