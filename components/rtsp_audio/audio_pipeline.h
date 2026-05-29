#pragma once

#include <cstddef>
#include <cstdint>

#include "biquad.h"
#include "dc_blocker.h"
#include "gain.h"
#include "high_cut_biquad.h"
#include "low_cut_biquad.h"
#include "soft_limiter.h"

namespace esphome::rtsp_audio::internal {

// Single per-sample DSP loop that the RTP send path runs over each
// packet's payload. Lives here (not inline in rtsp_audio.cpp) so the
// list of audio stages can grow — soft limiter, gain smoothing — without
// bloating the RTP packetiser. Pure logic, no ESPHome dependencies, so
// the host test build can exercise it directly.
//
// Stage order (matches the block diagram in docs/configuration.md):
//   sample -> dc_blocker_step    (always on)
//          -> low-cut biquad     (bypassable: lowcut_bypass)
//          -> high-cut biquad    (bypassable: highcut_bypass)
//          -> gain_apply_q8      (bypassable: gain == GAIN_Q8_UNITY)
//          -> soft_limiter_step  (bypassable: sl_bypass)
//          -> peak tap -> byteswap -> store back
//
// The DC blocker always runs first — its job is to kill the MEMS
// capsule's DC offset before any other stage sees it, so even when
// the user bypasses the low-cut the rest of the chain doesn't have
// to cope with a biased signal. One MAC per sample, sub-audible
// cutoff (5 Hz).
//
// The low-cut, high-cut, gain, and soft-limiter stages each have a
// bypass that the pipeline short-circuits. With all four bypassed the
// pipeline is bit-identical to a build without those features (DC
// blocker aside).
//
// The peak tap reads the post-gain sample (so it reflects what leaves
// on the wire, including clipping the gain stage introduces) and
// returns the per-packet max |sample|. Adding a single
// compare-and-update per sample is cheap and does not change the bytes
// produced.
//
// The byteswap is a local `__builtin_bswap16` rather than ESPHome's
// `convert_big_endian()` so this header has no transitive ESPHome
// includes; on every platform we target both compile to the same single
// instruction.

inline uint16_t byteswap_u16(uint16_t v) {
  return __builtin_bswap16(v);
}

// `s < 0 ? -(int32_t)s : s` avoids INT16_MIN's negation overflow
// (`-INT16_MIN` is undefined in int16 / int but well-defined in int32).
inline uint16_t abs_i16(int16_t s) {
  return static_cast<uint16_t>(s < 0 ? -static_cast<int32_t>(s) : s);
}

inline uint16_t process_l16_payload_inplace(int16_t *samples, size_t count, DcBlockerState &dc_state,
                                            BiquadState &lc_state, const BiquadCoeffs &lc_coeffs, bool lowcut_bypass,
                                            BiquadState &hc_state, const BiquadCoeffs &hc_coeffs, bool highcut_bypass,
                                            int32_t gain_q8, SoftLimiterState &sl_state, bool sl_bypass,
                                            float sl_threshold_linear, float sl_attack_coeff, float sl_release_coeff) {
  const bool apply_lowcut = !lowcut_bypass;
  const bool apply_highcut = !highcut_bypass;
  const bool apply_gain = (gain_q8 != GAIN_Q8_UNITY);
  const bool apply_limiter = !sl_bypass;
  uint16_t peak_abs = 0;
  float sl_min_gain = 1.0f;

  for (size_t i = 0; i < count; i++) {
    int16_t s = dc_blocker_step(samples[i], dc_state);
    if (apply_lowcut)
      s = biquad_step(s, lc_state, lc_coeffs);
    if (apply_highcut)
      s = biquad_step(s, hc_state, hc_coeffs);
    if (apply_gain)
      s = gain_apply_q8(s, gain_q8);
    if (apply_limiter) {
      s = soft_limiter_step(s, sl_state, sl_threshold_linear, sl_attack_coeff, sl_release_coeff);
      if (sl_state.last_gain < sl_min_gain)
        sl_min_gain = sl_state.last_gain;
    }
    const uint16_t a = abs_i16(s);
    if (a > peak_abs)
      peak_abs = a;
    samples[i] = static_cast<int16_t>(byteswap_u16(static_cast<uint16_t>(s)));
  }
  if (apply_limiter)
    sl_state.packet_min_gain = sl_min_gain;
  return peak_abs;
}

}  // namespace esphome::rtsp_audio::internal
