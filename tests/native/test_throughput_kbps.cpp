#include <gtest/gtest.h>

#include <cstdint>

#include "throughput_rate.h"

namespace esphome::rtsp_audio::internal {
namespace {

constexpr uint32_t WINDOW_5S_US = 5'000'000;

TEST(BytesWindowToKbps, ZeroDeltaIsZeroKbps) {
  EXPECT_EQ(0u, bytes_window_to_kbps(0, WINDOW_5S_US));
}

TEST(BytesWindowToKbps, ZeroWindowIsZeroKbps) {
  // Defensive against a caller passing window_us=0 (e.g. a stats tick that
  // fired in the same microsecond as the previous one). We'd rather publish
  // 0 than divide by zero.
  EXPECT_EQ(0u, bytes_window_to_kbps(40'000, 0));
}

TEST(BytesWindowToKbps, TypicalL16MonoStreamRoundsToExpectedBitrate) {
  // 32 kHz * 16-bit mono = 64000 B/s payload. RTP adds 12 B per 20 ms packet
  // = 600 B/s overhead, giving ~64600 B/s = ~516.8 kbps. Over a 5 s window
  // that's 323000 B; the helper should round to 517 kbps.
  EXPECT_EQ(517u, bytes_window_to_kbps(323'000, WINDOW_5S_US));
}

TEST(BytesWindowToKbps, RoundsHalfKbpsUp) {
  // 312 bytes over 5 s = 0.4992 kbps -> 0
  EXPECT_EQ(0u, bytes_window_to_kbps(312, WINDOW_5S_US));
  // 313 bytes over 5 s = 0.5008 kbps -> 1
  EXPECT_EQ(1u, bytes_window_to_kbps(313, WINDOW_5S_US));
}

TEST(BytesWindowToKbps, AcceptsModularSubtractedDelta) {
  // The call site computes delta via uint32 modular subtraction:
  //   delta = bytes_sent_ - stats_last_bytes_
  // so a 4 GB wrap of bytes_sent_ between ticks must look identical to a
  // small no-wrap delta. Simulate: counter wrapped from UINT32_MAX-49 to 50,
  // giving a true increase of 100 bytes.
  const uint32_t now = 50;
  const uint32_t prev = UINT32_MAX - 49;
  const uint32_t delta = now - prev;
  EXPECT_EQ(100u, delta);
  // 100 bytes over 5 s = 160 bps = 0.16 kbps -> 0
  EXPECT_EQ(0u, bytes_window_to_kbps(delta, WINDOW_5S_US));
}

TEST(BytesWindowToKbps, NearUint16MaxFitsExactly) {
  // 65535 kbps over a 5 s window = 65535 * 5000 / 8 = 40'959'375 bytes.
  EXPECT_EQ(65535u, bytes_window_to_kbps(40'959'375, WINDOW_5S_US));
}

TEST(BytesWindowToKbps, SaturatesAtUint16Max) {
  // Anything that would compute > 65535 kbps clamps to UINT16_MAX, so the
  // sensor never wraps silently. Pick a delta well past the boundary.
  EXPECT_EQ(65535u, bytes_window_to_kbps(50'000'000, WINDOW_5S_US));
  // And the absurd-input case: ~UINT32_MAX bytes in a 5 s window would
  // mathematically be ~6.87 Gbps; helper must still return UINT16_MAX,
  // not overflow or UB.
  EXPECT_EQ(65535u, bytes_window_to_kbps(UINT32_MAX, WINDOW_5S_US));
}

TEST(BytesWindowToKbps, WorksForNonDefaultWindow) {
  // Lock in that the helper is window-agnostic (the call site passes a
  // constant today, but the contract isn't tied to 5 s). 1 s window with
  // 64000 B = 512 kbps.
  EXPECT_EQ(512u, bytes_window_to_kbps(64'000, 1'000'000));
}

}  // namespace
}  // namespace esphome::rtsp_audio::internal
