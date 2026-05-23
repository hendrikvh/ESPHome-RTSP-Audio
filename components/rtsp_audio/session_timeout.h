#pragma once

#include <cstdint>

namespace esphome::rtsp_audio::internal {

// True if `now_usec - last_activity_usec` exceeds `timeout_seconds`.
// Pure function, no ESPHome dependencies, so it can be unit-tested on
// the host. Callers must pass monotonic timestamps (the production path
// uses `esp_timer_get_time()`); behavior on a non-monotonic clock is
// not part of the contract.
inline bool session_is_idle(int64_t now_usec, int64_t last_activity_usec, uint32_t timeout_seconds) {
  return (now_usec - last_activity_usec) > static_cast<int64_t>(timeout_seconds) * 1'000'000;
}

}  // namespace esphome::rtsp_audio::internal
