#pragma once

#include "esphome/core/defines.h"
#ifdef USE_RTSP_AUDIO
#ifdef USE_SWITCH

#include "esphome/components/switch/switch.h"
#include "esphome/core/component.h"
#include "rtsp_audio.h"

namespace esphome::rtsp_audio {

/// Home Assistant `switch` entity that enables or disables the soft
/// limiter stage on the parent RTSP component. The state is persisted
/// via ESPHome's standard switch restore mechanism (RESTORE_DEFAULT_OFF,
/// so the limiter is off on a fresh flash and remembers the last user
/// setting on subsequent boots).
class RtspAudioSoftLimiterSwitch : public switch_::Switch, public Component {
 public:
  void set_parent(RtspAudioComponent *p) { this->parent_ = p; }

  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  void write_state(bool state) override;

  RtspAudioComponent *parent_{nullptr};
};

}  // namespace esphome::rtsp_audio

#endif  // USE_SWITCH
#endif  // USE_RTSP_AUDIO
