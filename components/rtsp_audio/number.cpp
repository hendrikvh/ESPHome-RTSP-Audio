#include "number.h"

#ifdef USE_RTSP_AUDIO
#ifdef USE_NUMBER

#include "esphome/core/log.h"

namespace esphome::rtsp_audio {

static const char *const TAG = "rtsp_audio.number";

void RtspAudioLowCutFilterNumber::setup() {
  float value = this->initial_value_;
  if (this->restore_value_) {
    this->pref_ = global_preferences->make_preference<float>(this->get_object_id_hash());
    float stored;
    if (this->pref_.load(&stored))
      value = stored;
  }
  // Push through the parent first so the filter is using the restored
  // value before we publish to HA; otherwise a brief transient could
  // hit the wire at the default frequency.
  if (this->parent_ != nullptr)
    this->parent_->set_lowcut_filter_frequency_hz(value);
  this->publish_state(value);
}

void RtspAudioLowCutFilterNumber::dump_config() {
  LOG_NUMBER("", "RTSP audio low-cut filter frequency", this);
  ESP_LOGCONFIG(TAG, "  Initial value: %.1f Hz", this->initial_value_);
  ESP_LOGCONFIG(TAG, "  Restore value: %s", YESNO(this->restore_value_));
}

void RtspAudioLowCutFilterNumber::control(float value) {
  if (this->parent_ != nullptr)
    this->parent_->set_lowcut_filter_frequency_hz(value);
  this->publish_state(value);
  if (this->restore_value_)
    this->pref_.save(&value);
}

void RtspAudioGainDbNumber::setup() {
  float value = this->initial_value_;
  if (this->restore_value_) {
    this->pref_ = global_preferences->make_preference<float>(this->get_object_id_hash());
    float stored;
    if (this->pref_.load(&stored))
      value = stored;
  }
  // Push through the parent first so the gain stage is already at the
  // restored value when the first session starts; publishing afterwards
  // keeps HA in sync.
  if (this->parent_ != nullptr)
    this->parent_->set_gain_db(value);
  this->publish_state(value);
}

void RtspAudioGainDbNumber::dump_config() {
  LOG_NUMBER("", "RTSP audio input gain", this);
  ESP_LOGCONFIG(TAG, "  Initial value: %+.1f dB", this->initial_value_);
  ESP_LOGCONFIG(TAG, "  Restore value: %s", YESNO(this->restore_value_));
}

void RtspAudioGainDbNumber::control(float value) {
  if (this->parent_ != nullptr)
    this->parent_->set_gain_db(value);
  this->publish_state(value);
  if (this->restore_value_)
    this->pref_.save(&value);
}

}  // namespace esphome::rtsp_audio

#endif  // USE_NUMBER
#endif  // USE_RTSP_AUDIO
