#pragma once

#include "esphome/core/defines.h"
#ifdef USE_RTSP_AUDIO
#ifdef USE_NUMBER

#include "esphome/components/number/number.h"
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "rtsp_audio.h"

namespace esphome::rtsp_audio {

/// Home Assistant `number` entity bound to the parent RTSP component's
/// low-cut filter (a.k.a. DC blocker / high-pass) cutoff frequency.
/// Restoring the persisted value on boot calls
/// `parent_->set_lowcut_filter_frequency_hz()` so the filter starts at
/// the last-known cutoff without any user action.
class RtspAudioLowCutFilterNumber : public number::Number, public Component {
 public:
  void set_parent(RtspAudioComponent *p) { this->parent_ = p; }
  void set_initial_value(float v) { this->initial_value_ = v; }
  void set_restore_value(bool b) { this->restore_value_ = b; }

  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  void control(float value) override;

  RtspAudioComponent *parent_{nullptr};
  float initial_value_{100.0f};
  bool restore_value_{true};
  ESPPreferenceObject pref_;
};

/// Home Assistant `number` entity bound to the parent RTSP component's
/// high-cut filter (one-pole IIR low-pass) cutoff frequency. Mirrors
/// `RtspAudioLowCutFilterNumber`: the persisted value is pushed to the
/// parent before being published, so the very first RTP packet of a
/// session already uses the restored cutoff. A cutoff at the max
/// (20 kHz) disables the filter via the bit-identical fast path.
class RtspAudioHighCutFilterNumber : public number::Number, public Component {
 public:
  void set_parent(RtspAudioComponent *p) { this->parent_ = p; }
  void set_initial_value(float v) { this->initial_value_ = v; }
  void set_restore_value(bool b) { this->restore_value_ = b; }

  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  void control(float value) override;

  RtspAudioComponent *parent_{nullptr};
  float initial_value_{20000.0f};  // default off
  bool restore_value_{true};
  ESPPreferenceObject pref_;
};

/// Home Assistant `number` entity bound to the parent RTSP component's
/// software input gain, in dB. Mirrors `RtspAudioLowCutFilterNumber`:
/// the persisted dB value is pushed to the parent via `set_gain_db()`
/// before being published, so the very first RTP packet of a session
/// already uses the restored gain.
class RtspAudioGainDbNumber : public number::Number, public Component {
 public:
  void set_parent(RtspAudioComponent *p) { this->parent_ = p; }
  void set_initial_value(float v) { this->initial_value_ = v; }
  void set_restore_value(bool b) { this->restore_value_ = b; }

  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  void control(float value) override;

  RtspAudioComponent *parent_{nullptr};
  float initial_value_{0.0f};  // 0 dB == unity
  bool restore_value_{true};
  ESPPreferenceObject pref_;
};

}  // namespace esphome::rtsp_audio

#endif  // USE_NUMBER
#endif  // USE_RTSP_AUDIO
