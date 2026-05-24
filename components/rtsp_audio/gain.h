#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace esphome::rtsp_audio::internal {

// Software input gain stage applied per sample, after the DC blocker and
// just before the L16 byteswap. Q8 fixed point so the inner loop is a
// single integer multiply and an arithmetic shift on ESP32 / S2 (no FPU
// per sample). The float-to-Q8 conversion only happens when the HA
// slider moves, well off the audio hot path.
//
// `GAIN_Q8_UNITY` (256) is reserved as a fast-path sentinel: when the
// stored gain equals this value, the pipeline skips the multiply entirely
// so the default install is bit-identical to a no-gain build.

constexpr int32_t GAIN_Q8_UNITY = 256;
constexpr float GAIN_MIN = 0.1f;
constexpr float GAIN_MAX = 80.0f;
constexpr float GAIN_DEFAULT = 1.0f;

// Converts a linear multiplier to Q8. Clamped to [GAIN_MIN, GAIN_MAX] so
// an out-of-range HA control can't produce an extreme coefficient. The
// Python schema clamps too; this is belt-and-braces against a
// hand-written `number.set` action.
inline int32_t gain_q8_for(float linear) {
  const float clamped = std::clamp(linear, GAIN_MIN, GAIN_MAX);
  return static_cast<int32_t>(std::lround(clamped * 256.0f));
}

// Applies a Q8 gain to one int16 sample with saturating clamp. The
// intermediate is int32 because at GAIN_MAX (Q8 = 4096) the product
// overflows int16 by orders of magnitude on loud input — we want
// saturation, never wrap.
inline int16_t gain_apply_q8(int16_t sample, int32_t gain_q8) {
  const int32_t scaled = (static_cast<int32_t>(sample) * gain_q8) >> 8;
  return static_cast<int16_t>(std::clamp<int32_t>(scaled, INT16_MIN, INT16_MAX));
}

}  // namespace esphome::rtsp_audio::internal
