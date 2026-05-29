#include "switch.h"

#ifdef USE_RTSP_AUDIO
#ifdef USE_SWITCH

#include "esphome/core/log.h"

namespace esphome::rtsp_audio {

static const char *const TAG = "rtsp_audio.switch";

void RtspAudioSoftLimiterSwitch::setup() {
  // Restore the persisted state (RESTORE_DEFAULT_OFF → off on first boot).
  // write_state() pushes the result to the parent so the limiter is
  // already in the right state before the first session starts.
  auto restored = this->get_initial_state_with_restore_mode();
  this->write_state(restored.has_value() ? restored.value() : false);
}

void RtspAudioSoftLimiterSwitch::dump_config() {
  LOG_SWITCH("", "Soft limiter", this);
}

void RtspAudioSoftLimiterSwitch::write_state(bool state) {
  if (this->parent_ != nullptr)
    this->parent_->set_soft_limiter_enabled(state);
  this->publish_state(state);
}

}  // namespace esphome::rtsp_audio

#endif  // USE_SWITCH
#endif  // USE_RTSP_AUDIO
