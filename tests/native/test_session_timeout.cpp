#include <gtest/gtest.h>

#include "session_timeout.h"

namespace esphome::rtsp_audio::internal {
namespace {

constexpr int64_t USEC_PER_SEC = 1'000'000;

TEST(SessionIsIdle, ReturnsFalseJustBelowThreshold) {
  const int64_t last = 0;
  const uint32_t timeout = 60;
  const int64_t now = static_cast<int64_t>(timeout) * USEC_PER_SEC - 1;
  EXPECT_FALSE(session_is_idle(now, last, timeout));
}

TEST(SessionIsIdle, ReturnsFalseExactlyAtThreshold) {
  // Decision is strictly greater-than, so equality is "not idle yet".
  const int64_t last = 0;
  const uint32_t timeout = 60;
  const int64_t now = static_cast<int64_t>(timeout) * USEC_PER_SEC;
  EXPECT_FALSE(session_is_idle(now, last, timeout));
}

TEST(SessionIsIdle, ReturnsTrueOneMicrosecondPastThreshold) {
  const int64_t last = 0;
  const uint32_t timeout = 60;
  const int64_t now = static_cast<int64_t>(timeout) * USEC_PER_SEC + 1;
  EXPECT_TRUE(session_is_idle(now, last, timeout));
}

TEST(SessionIsIdle, ReturnsTrueForLargeGap) {
  const int64_t last = 1'000;
  const uint32_t timeout = 60;
  const int64_t now = last + 24LL * 3600 * USEC_PER_SEC;  // a day later
  EXPECT_TRUE(session_is_idle(now, last, timeout));
}

TEST(SessionIsIdle, ZeroTimeoutIsIdleForAnyPositiveGap) {
  EXPECT_TRUE(session_is_idle(1, 0, 0));
  EXPECT_TRUE(session_is_idle(USEC_PER_SEC, 0, 0));
}

TEST(SessionIsIdle, ZeroTimeoutNotIdleWhenNoTimePassed) {
  EXPECT_FALSE(session_is_idle(0, 0, 0));
  EXPECT_FALSE(session_is_idle(12345, 12345, 0));
}

TEST(SessionIsIdle, UsesAbsoluteTimestampsNotJustDelta) {
  // Same delta against different bases should produce the same answer —
  // sanity-checks that we aren't accidentally treating `last` as 0.
  const uint32_t timeout = 1;
  const int64_t gap = 2 * USEC_PER_SEC;
  EXPECT_TRUE(session_is_idle(gap, 0, timeout));
  EXPECT_TRUE(session_is_idle(1'000'000'000 + gap, 1'000'000'000, timeout));
}

}  // namespace
}  // namespace esphome::rtsp_audio::internal
