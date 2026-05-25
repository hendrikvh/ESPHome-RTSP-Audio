#pragma once

namespace esphome {
namespace rtsp_audio {
namespace internal {

// Tracks the "ring buffer / RTP packet free is deferred until the mic task
// has actually exited" handshake used by close_session_() / loop().
//
// MicrophoneSource::stop() is asynchronous: the mic FreeRTOS task can
// deliver trailing data callbacks that write into the ring buffer for
// tens of milliseconds after stop() returns. close_session_() arms the
// guard; loop() polls it each iteration with the current is_stopped()
// observation and performs the real free only once the mic has reported
// stopped. A rapid reconnect cancels the guard so the new session keeps
// the existing buffers.
class TeardownGuard {
 public:
  void arm() { pending_ = true; }
  void cancel() { pending_ = false; }
  bool pending() const { return pending_; }

  // Returns true exactly once per arm() — on the first poll where
  // mic_is_stopped is true. Caller performs the deallocate on a true
  // return. Subsequent polls return false until arm() is called again.
  bool poll(bool mic_is_stopped) {
    if (pending_ && mic_is_stopped) {
      pending_ = false;
      return true;
    }
    return false;
  }

 private:
  bool pending_{false};
};

}  // namespace internal
}  // namespace rtsp_audio
}  // namespace esphome
