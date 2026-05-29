#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace esphome::rtsp_audio::internal {

// Post-gain peak limiter with 1-pole IIR envelope follower.
//
// State: a single float holding the current peak-envelope estimate
// (normalised to 0..1). Reset to zero at the start of each PLAY so a
// new session doesn't inherit the previous one's gain-reduction state.
//
// Algorithm (per sample):
//   1. Normalise int16 to ±1.0 float.
//   2. Update envelope: fast attack on rising edge, slow release on falling.
//   3. If envelope exceeds threshold_linear, apply proportional gain
//      reduction (threshold / envelope) to keep the output near the
//      threshold ceiling rather than flat-top clipping above it.
//   4. Re-scale to int16; safety clamp catches edge cases where
//      floating-point rounding pushes a single sample fractionally past
//      ±32767 (gain is ≤ 1.0 so this rarely fires).
//
// Coefficients are computed once when a slider moves, not per sample:
//   coeff = exp(-1 / (time_ms * 0.001 * sample_rate_hz))
// Short time constant → coeff near 0 (fast response).
// Long time constant  → coeff near 1 (slow response).
//
// The limiter sits after the gain stage in the DSP chain:
//   gain_apply_q8 → soft_limiter_step → peak_tap → byteswap
// The hard saturating clamp in gain_apply_q8 remains as the absolute
// INT16 ceiling; the soft limiter's threshold sits below it so loud
// transients are caught here first, before the harder clip fires.

struct SoftLimiterState {
  float env{0.0f};
  // Gain applied to the last processed sample. Written by soft_limiter_step()
  // so the pipeline can track the per-packet minimum without an extra parameter.
  float last_gain{1.0f};
  // Minimum gain applied across the most recent process_l16_payload_inplace()
  // call. Written by the pipeline at the end of each packet; read by
  // rtsp_audio.cpp to accumulate the per-window minimum for the HA sensor.
  float packet_min_gain{1.0f};
};

// HA-facing constants — mirrored in number.py.
constexpr float SOFT_LIMITER_THRESHOLD_DB_MIN = -20.0f;
constexpr float SOFT_LIMITER_THRESHOLD_DB_MAX = 0.0f;
constexpr float SOFT_LIMITER_THRESHOLD_DB_DEFAULT = -3.0f;
constexpr float SOFT_LIMITER_THRESHOLD_DB_STEP = 1.0f;

constexpr float SOFT_LIMITER_ATTACK_MS_MIN = 0.1f;
constexpr float SOFT_LIMITER_ATTACK_MS_MAX = 50.0f;
constexpr float SOFT_LIMITER_ATTACK_MS_DEFAULT = 5.0f;
constexpr float SOFT_LIMITER_ATTACK_MS_STEP = 0.1f;

constexpr float SOFT_LIMITER_RELEASE_MS_MIN = 10.0f;
constexpr float SOFT_LIMITER_RELEASE_MS_MAX = 2000.0f;
constexpr float SOFT_LIMITER_RELEASE_MS_DEFAULT = 100.0f;
constexpr float SOFT_LIMITER_RELEASE_MS_STEP = 10.0f;

// dBFS → linear amplitude (0.0 to 1.0). Called when the threshold
// slider moves, not per sample.
inline float limiter_db_to_linear(float db) {
  return std::pow(10.0f, db / 20.0f);
}

// Compute the 1-pole IIR time-constant coefficient.
// time_ms = 0 → instantaneous (coeff = 0).
inline float limiter_time_coeff(float time_ms, float sample_rate_hz) {
  if (time_ms <= 0.0f || sample_rate_hz <= 0.0f)
    return 0.0f;
  return std::exp(-1.0f / (time_ms * 0.001f * sample_rate_hz));
}

// Process one int16 sample through the peak limiter.
// Mutates `state` in place. Coefficients must be pre-computed via
// `limiter_time_coeff`; threshold_linear via `limiter_db_to_linear`.
inline int16_t soft_limiter_step(int16_t sample, SoftLimiterState &state, float threshold_linear, float attack_coeff,
                                  float release_coeff) {
  // Normalise to ±1.0 using 32768 so INT16_MIN maps to exactly -1.0
  // (using 32767 would leave +1.0 unreachable for positive INT16_MAX).
  constexpr float kScale = 1.0f / 32768.0f;
  const float x = static_cast<float>(sample) * kScale;
  const float abs_x = x < 0.0f ? -x : x;

  // Envelope follower: attack on rise, release on fall.
  if (abs_x > state.env) {
    state.env = attack_coeff * state.env + (1.0f - attack_coeff) * abs_x;
  } else {
    state.env = release_coeff * state.env;
  }

  // Proportional gain reduction when envelope exceeds threshold.
  float gain = 1.0f;
  if (state.env > threshold_linear && state.env > 0.0f) {
    gain = threshold_linear / state.env;
  }
  state.last_gain = gain;

  // Re-scale back to int16. Safety clamp is rarely needed since gain ≤ 1.0,
  // but floating-point edge cases (e.g. denormals, rounding on INT16_MIN)
  // make it worth keeping.
  const int32_t out = static_cast<int32_t>(x * gain * 32768.0f);
  return static_cast<int16_t>(std::clamp<int32_t>(out, INT16_MIN, INT16_MAX));
}

}  // namespace esphome::rtsp_audio::internal
