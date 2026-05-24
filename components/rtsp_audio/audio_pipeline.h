#pragma once

#include <cstddef>
#include <cstdint>

#include "dc_blocker.h"
#include "gain.h"

namespace esphome::rtsp_audio::internal {

// Single per-sample DSP loop that the RTP send path runs over each
// packet's payload. Lives here (not inline in rtsp_audio.cpp) so the
// list of audio stages can grow — soft limiter, gain smoothing — without
// bloating the RTP packetiser. Pure logic, no ESPHome dependencies, so
// the host test build can exercise it directly.
//
// Stage order (matches the block diagram in docs/configuration.md):
//   sample -> dc_blocker_step -> gain_apply_q8 -> byteswap -> store back
//
// The byteswap is a local `__builtin_bswap16` rather than ESPHome's
// `convert_big_endian()` so this header has no transitive ESPHome
// includes; on every platform we target both compile to the same single
// instruction.

inline uint16_t byteswap_u16(uint16_t v) {
  return __builtin_bswap16(v);
}

inline void process_l16_payload_inplace(int16_t *samples, size_t count, DcBlockerState &dc_state, int32_t lowcut_r_q15,
                                        int32_t gain_q8) {
  // Unity-gain fast path: bit-identical to the pre-gain build so an
  // un-touched HA install streams exactly the same bytes as before.
  if (gain_q8 == GAIN_Q8_UNITY) {
    for (size_t i = 0; i < count; i++) {
      const int16_t filtered = dc_blocker_step(samples[i], dc_state, lowcut_r_q15);
      samples[i] = static_cast<int16_t>(byteswap_u16(static_cast<uint16_t>(filtered)));
    }
    return;
  }

  for (size_t i = 0; i < count; i++) {
    const int16_t filtered = dc_blocker_step(samples[i], dc_state, lowcut_r_q15);
    const int16_t scaled = gain_apply_q8(filtered, gain_q8);
    samples[i] = static_cast<int16_t>(byteswap_u16(static_cast<uint16_t>(scaled)));
  }
}

}  // namespace esphome::rtsp_audio::internal
