#pragma once

#include <algorithm>
#include <cstdint>

namespace esphome::rtsp_audio::internal {

// Direct-form-I biquad in `float`. Shared between the low-cut
// (Butterworth high-pass) and high-cut (Butterworth low-pass) stages.
//
//   y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]
//
// Float was the only sane choice here once we ran the numbers. A
// fixed-point Q30 biquad has classic IIR limit cycles at a 100 Hz
// cutoff at 32 kHz fs: the feedback poles sit at radius ≈ 0.986, the
// per-step state change at DC works out below one Q30 LSB, and the
// state gets stuck at a non-zero value forever (~-28 dBFS on a 1000-LSB
// DC input, ~-1200 LSB out for the default low-cut). 64-bit
// accumulators don't fix it because the quantization happens at the
// state, not the accumulator; you'd need int128 to do Q30 × Q30 → Q60
// feedback without precision loss.
//
// Single-precision float has 24 bits of mantissa, plenty for an audio
// biquad, and gives us the same audio cost on every supported ESP
// target — ESP32-S3 uses its FPU, S2 and C3 use software float
// (slower but still bounded). We chose this over a faster fixed-point
// path that would only work on FPU-equipped parts, since the
// component is meant to ship to all of them.

struct BiquadCoeffs {
  float b0, b1, b2;
  float a1, a2;
};

struct BiquadState {
  float x1, x2;
  float y1, y2;
};

// Per-sample step. State is held in float so the feedback path retains
// full precision; the int16 output is saturated at the I/O boundary.
inline int16_t biquad_step(int16_t x, BiquadState &s, const BiquadCoeffs &c) {
  const float xf = static_cast<float>(x);
  const float y = c.b0 * xf + c.b1 * s.x1 + c.b2 * s.x2 - c.a1 * s.y1 - c.a2 * s.y2;
  s.x2 = s.x1;
  s.x1 = xf;
  s.y2 = s.y1;
  s.y1 = y;
  // Round-to-nearest on the output quantization; clamp to int16 range
  // (the unclamped float y is fed back so transient response is intact).
  const float rounded = y >= 0.0f ? y + 0.5f : y - 0.5f;
  const int32_t y_int = static_cast<int32_t>(rounded);
  return static_cast<int16_t>(std::clamp<int32_t>(y_int, INT16_MIN, INT16_MAX));
}

}  // namespace esphome::rtsp_audio::internal
