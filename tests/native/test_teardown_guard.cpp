#include <gtest/gtest.h>

#include "teardown_guard.h"

namespace esphome::rtsp_audio::internal {
namespace {

TEST(TeardownGuard, IdleByDefault) {
  TeardownGuard g;
  EXPECT_FALSE(g.pending());
  // Polling without arming never asks the caller to free.
  EXPECT_FALSE(g.poll(true));
  EXPECT_FALSE(g.poll(false));
}

TEST(TeardownGuard, PollIsFalseWhileMicStillStopping) {
  // Models the race window the fix targets: close_session_() has armed
  // the guard, but the mic task hasn't exited yet — so trailing
  // callbacks could still write into the ring buffer. Returning true
  // here would be the original use-after-free.
  TeardownGuard g;
  g.arm();
  EXPECT_TRUE(g.pending());
  for (int i = 0; i < 5; ++i) {
    EXPECT_FALSE(g.poll(false));
    EXPECT_TRUE(g.pending());
  }
}

TEST(TeardownGuard, PollIsTrueOnceMicReportsStopped) {
  TeardownGuard g;
  g.arm();
  EXPECT_FALSE(g.poll(false));
  EXPECT_TRUE(g.poll(true));
  EXPECT_FALSE(g.pending());
}

TEST(TeardownGuard, PollFiresExactlyOncePerArm) {
  // The real loop() calls deallocate_stream_buffers_() on a true return.
  // A second true return would double-free, so the guard must latch off.
  TeardownGuard g;
  g.arm();
  EXPECT_TRUE(g.poll(true));
  EXPECT_FALSE(g.poll(true));
  EXPECT_FALSE(g.poll(true));
}

TEST(TeardownGuard, CancelBeforeStopPreventsDealloc) {
  // Rapid reconnect path: allocate_stream_buffers_() calls cancel()
  // before the mic task has finished draining. The existing buffers
  // must stay alive for the new session.
  TeardownGuard g;
  g.arm();
  g.cancel();
  EXPECT_FALSE(g.pending());
  EXPECT_FALSE(g.poll(true));
}

TEST(TeardownGuard, CancelAfterStopAlsoSafe) {
  // If cancel() races slightly behind the mic stopping, the next poll
  // still must not fire — nothing was armed at the time of the poll.
  TeardownGuard g;
  g.arm();
  g.cancel();
  EXPECT_FALSE(g.poll(true));
}

TEST(TeardownGuard, ReArmAfterFireWorks) {
  // A subsequent session close must be able to defer again.
  TeardownGuard g;
  g.arm();
  EXPECT_TRUE(g.poll(true));

  g.arm();
  EXPECT_FALSE(g.poll(false));
  EXPECT_TRUE(g.poll(true));
}

TEST(TeardownGuard, ArmIsIdempotent) {
  TeardownGuard g;
  g.arm();
  g.arm();
  g.arm();
  EXPECT_TRUE(g.pending());
  EXPECT_TRUE(g.poll(true));
  EXPECT_FALSE(g.poll(true));
}

TEST(TeardownGuard, RealisticSessionLifecycle) {
  // Walks the exact sequence the component performs:
  //   PLAY     → buffers allocated, guard idle
  //   TEARDOWN → close_session_() arms the guard
  //   loop 1   → mic still draining, no free
  //   loop 2   → mic still draining, no free
  //   loop 3   → mic_source_->is_stopped() flips true, free fires
  //   loop 4+  → no further frees (no double-free)
  TeardownGuard g;
  EXPECT_FALSE(g.poll(true));  // PLAY: nothing to do.

  g.arm();  // TEARDOWN.

  bool freed = false;
  for (int loop_iter = 0; loop_iter < 10; ++loop_iter) {
    const bool mic_stopped = loop_iter >= 3;
    if (g.poll(mic_stopped)) {
      EXPECT_FALSE(freed) << "double-free at iter " << loop_iter;
      freed = true;
    }
  }
  EXPECT_TRUE(freed);
}

}  // namespace
}  // namespace esphome::rtsp_audio::internal
