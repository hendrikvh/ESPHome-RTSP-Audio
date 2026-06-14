#pragma once

#include <cstdint>

namespace esphome::rtsp_audio::internal {

// Converts a byte delta over a fixed window into a network-side bitrate in
// kbit/s, rounded to the nearest integer and saturated to UINT16_MAX.
// Pure function, no ESPHome dependencies, so it can be unit-tested on the
// host. Callers feed `delta_bytes` already computed via uint32_t modular
// subtraction, so a 4 GB wrap of the underlying byte counter is transparent
// to the helper.
inline uint16_t bytes_window_to_kbps(uint32_t delta_bytes, uint32_t window_us) {
  if (window_us == 0)
    return 0;
  const uint64_t bits = static_cast<uint64_t>(delta_bytes) * 8u;
  const uint64_t kbps = (bits * 1000ull + window_us / 2) / window_us;
  if (kbps > UINT16_MAX)
    return UINT16_MAX;
  return static_cast<uint16_t>(kbps);
}

}  // namespace esphome::rtsp_audio::internal
