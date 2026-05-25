#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace esphome::rtsp_audio::internal {

// Software audio gain stage applied per sample, after the DC blocker and
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
constexpr float GAIN_MAX = 100.0f;
constexpr float GAIN_DEFAULT = 1.0f;

// HA-facing units. The slider speaks dB; the per-sample loop still
// multiplies by a Q8 linear coefficient. 0 dB lands exactly on the
// GAIN_Q8_UNITY fast path, so the default install stays bit-identical.
constexpr float GAIN_DB_MIN = -20.0f;
constexpr float GAIN_DB_MAX = 40.0f;
constexpr float GAIN_DB_DEFAULT = 0.0f;

// Converts a linear multiplier to Q8. Clamped to [GAIN_MIN, GAIN_MAX] so
// an out-of-range HA control can't produce an extreme coefficient. The
// Python schema clamps too; this is belt-and-braces against a
// hand-written `number.set` action.
inline int32_t gain_q8_for(float linear) {
  const float clamped = std::clamp(linear, GAIN_MIN, GAIN_MAX);
  return static_cast<int32_t>(std::lround(clamped * 256.0f));
}

// dB → linear amplitude. Pure float; called when the slider moves, not
// per sample. `gain_q8_for(...)` further clamps to the linear bounds
// so an out-of-range dB value can't produce an extreme Q8.
inline float db_to_linear(float db) {
  return std::pow(10.0f, db / 20.0f);
}

// dB → Q8 in one step. Clamps dB to [GAIN_DB_MIN, GAIN_DB_MAX] first so
// the final Q8 lands inside the linear range supported by the per-sample
// loop, even if a caller hand-rolls a `number.set` action outside the
// HA slider bounds.
inline int32_t gain_q8_for_db(float db) {
  const float clamped_db = std::clamp(db, GAIN_DB_MIN, GAIN_DB_MAX);
  return gain_q8_for(db_to_linear(clamped_db));
}

// Linear → dB. Used only for the dump_config debug line; the audio hot
// path never calls this.
inline float linear_to_db(float linear) {
  return 20.0f * std::log10(linear);
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
