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
    this->parent_->set_low_cut_frequency_hz(value);
  this->publish_state(value);
}

void RtspAudioLowCutFilterNumber::dump_config() {
  LOG_NUMBER("", "Low cut frequency", this);
  ESP_LOGCONFIG(TAG, "  Initial value: %.1f Hz", this->initial_value_);
  ESP_LOGCONFIG(TAG, "  Restore value: %s", YESNO(this->restore_value_));
}

void RtspAudioLowCutFilterNumber::control(float value) {
  if (this->parent_ != nullptr)
    this->parent_->set_low_cut_frequency_hz(value);
  this->publish_state(value);
  if (this->restore_value_)
    this->pref_.save(&value);
}

void RtspAudioHighCutFilterNumber::setup() {
  float value = this->initial_value_;
  if (this->restore_value_) {
    this->pref_ = global_preferences->make_preference<float>(this->get_object_id_hash());
    float stored;
    if (this->pref_.load(&stored))
      value = stored;
  }
  if (this->parent_ != nullptr)
    this->parent_->set_high_cut_frequency_hz(value);
  this->publish_state(value);
}

void RtspAudioHighCutFilterNumber::dump_config() {
  LOG_NUMBER("", "High cut frequency", this);
  ESP_LOGCONFIG(TAG, "  Initial value: %.1f Hz", this->initial_value_);
  ESP_LOGCONFIG(TAG, "  Restore value: %s", YESNO(this->restore_value_));
}

void RtspAudioHighCutFilterNumber::control(float value) {
  if (this->parent_ != nullptr)
    this->parent_->set_high_cut_frequency_hz(value);
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
  LOG_NUMBER("", "RTSP audio gain", this);
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

void RtspAudioSoftLimiterThresholdNumber::setup() {
  float value = this->initial_value_;
  if (this->restore_value_) {
    this->pref_ = global_preferences->make_preference<float>(this->get_object_id_hash());
    float stored;
    if (this->pref_.load(&stored))
      value = stored;
  }
  if (this->parent_ != nullptr)
    this->parent_->set_soft_limiter_threshold_db(value);
  this->publish_state(value);
}

void RtspAudioSoftLimiterThresholdNumber::dump_config() {
  LOG_NUMBER("", "Soft limiter threshold", this);
  ESP_LOGCONFIG(TAG, "  Initial value: %.1f dBFS", this->initial_value_);
  ESP_LOGCONFIG(TAG, "  Restore value: %s", YESNO(this->restore_value_));
}

void RtspAudioSoftLimiterThresholdNumber::control(float value) {
  if (this->parent_ != nullptr)
    this->parent_->set_soft_limiter_threshold_db(value);
  this->publish_state(value);
  if (this->restore_value_)
    this->pref_.save(&value);
}

void RtspAudioSoftLimiterAttackMsNumber::setup() {
  float value = this->initial_value_;
  if (this->restore_value_) {
    this->pref_ = global_preferences->make_preference<float>(this->get_object_id_hash());
    float stored;
    if (this->pref_.load(&stored))
      value = stored;
  }
  if (this->parent_ != nullptr)
    this->parent_->set_soft_limiter_attack_ms(value);
  this->publish_state(value);
}

void RtspAudioSoftLimiterAttackMsNumber::dump_config() {
  LOG_NUMBER("", "Soft limiter attack time", this);
  ESP_LOGCONFIG(TAG, "  Initial value: %.1f ms", this->initial_value_);
  ESP_LOGCONFIG(TAG, "  Restore value: %s", YESNO(this->restore_value_));
}

void RtspAudioSoftLimiterAttackMsNumber::control(float value) {
  if (this->parent_ != nullptr)
    this->parent_->set_soft_limiter_attack_ms(value);
  this->publish_state(value);
  if (this->restore_value_)
    this->pref_.save(&value);
}

void RtspAudioSoftLimiterReleaseMsNumber::setup() {
  float value = this->initial_value_;
  if (this->restore_value_) {
    this->pref_ = global_preferences->make_preference<float>(this->get_object_id_hash());
    float stored;
    if (this->pref_.load(&stored))
      value = stored;
  }
  if (this->parent_ != nullptr)
    this->parent_->set_soft_limiter_release_ms(value);
  this->publish_state(value);
}

void RtspAudioSoftLimiterReleaseMsNumber::dump_config() {
  LOG_NUMBER("", "Soft limiter release time", this);
  ESP_LOGCONFIG(TAG, "  Initial value: %.1f ms", this->initial_value_);
  ESP_LOGCONFIG(TAG, "  Restore value: %s", YESNO(this->restore_value_));
}

void RtspAudioSoftLimiterReleaseMsNumber::control(float value) {
  if (this->parent_ != nullptr)
    this->parent_->set_soft_limiter_release_ms(value);
  this->publish_state(value);
  if (this->restore_value_)
    this->pref_.save(&value);
}

}  // namespace esphome::rtsp_audio

#endif  // USE_NUMBER
#endif  // USE_RTSP_AUDIO
