#pragma once

#include "esphome/core/defines.h"
#ifdef USE_RTSP_AUDIO

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "biquad.h"
#include "dc_blocker.h"
#include "esphome/components/audio/audio.h"
#include "esphome/components/microphone/microphone_source.h"
#include "esphome/components/socket/socket.h"
#include "esphome/core/component.h"
#include "esphome/core/ring_buffer.h"
#include "gain.h"
#include "high_cut_biquad.h"
#include "low_cut_biquad.h"
#include "soft_limiter.h"
#include "teardown_guard.h"

#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif

namespace esphome::rtsp_audio {

/// Single-client RTSP server that streams the configured `MicrophoneSource`
/// as RTP/AVP payload type 96 (`L16/<rate>/1`) over UDP (RFC 3551).
///
/// The component plugs the mic via `MicrophoneSource::add_data_callback`
/// into an `esphome::RingBuffer`, then drains the buffer in `loop()` at the
/// configured `packet_ms` cadence and emits RTP packets in network byte order.
class RtspAudioComponent : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

  void set_microphone_source(microphone::MicrophoneSource *mic) { this->mic_source_ = mic; }
  void set_listen_port(uint16_t port) { this->listen_port_ = port; }
  void set_packet_duration_ms(uint16_t ms) { this->packet_duration_ms_ = ms; }

  /// Updates the low-cut filter frequency (in Hz) at runtime. Called
  /// from the bundled `number` platform when the HA slider moves and
  /// from `Number::setup()` when the persisted value is restored on boot.
  /// Values below LOW_CUT_MIN_CUTOFF_HZ (20 Hz) disable the stage
  /// entirely (bit-identical bypass, mirroring the high-cut's
  /// max-cutoff bypass). Out-of-range values above are clamped to
  /// LOW_CUT_MAX_CUTOFF_HZ.
  void set_low_cut_frequency_hz(float hz);

  /// Updates the high-cut filter frequency (in Hz) at runtime. Called
  /// from the bundled `number` platform when the HA slider moves and
  /// from `Number::setup()` when the persisted value is restored on
  /// boot. Out-of-range values are clamped to
  /// [HIGH_CUT_MIN_CUTOFF_HZ, HIGH_CUT_MAX_CUTOFF_HZ]; a cutoff at the
  /// max disables the filter via the bit-identical fast path in the
  /// per-sample loop.
  void set_high_cut_frequency_hz(float hz);

  /// Updates the software audio gain (in dB) at runtime. Called from
  /// the bundled `number` platform on slider moves and on restore.
  /// Out-of-range values are clamped to
  /// [internal::GAIN_DB_MIN, internal::GAIN_DB_MAX]; 0 dB takes the
  /// bit-identical fast path in the per-sample loop. Internally the
  /// dB value is converted to a Q8 linear coefficient — the audio hot
  /// path never sees dB.
  void set_gain_db(float db);

  /// Enables or disables the soft limiter stage. Called from the
  /// bundled `switch` platform when the HA toggle changes and on
  /// restore at boot. Disabling sets the bypass flag so the stage is
  /// skipped entirely (bit-identical to a build without the limiter).
  void set_soft_limiter_enabled(bool enabled);

  /// Updates the limiter threshold (in dBFS). Out-of-range values are
  /// clamped to [SOFT_LIMITER_THRESHOLD_DB_MIN, SOFT_LIMITER_THRESHOLD_DB_MAX].
  void set_soft_limiter_threshold_db(float db);

  /// Updates the limiter attack time (in ms). Out-of-range values are
  /// clamped to [SOFT_LIMITER_ATTACK_MS_MIN, SOFT_LIMITER_ATTACK_MS_MAX].
  void set_soft_limiter_attack_ms(float ms);

  /// Updates the limiter release time (in ms). Out-of-range values are
  /// clamped to [SOFT_LIMITER_RELEASE_MS_MIN, SOFT_LIMITER_RELEASE_MS_MAX].
  void set_soft_limiter_release_ms(float ms);

#ifdef USE_BINARY_SENSOR
  void set_client_connected_binary_sensor(binary_sensor::BinarySensor *s) { this->client_connected_bs_ = s; }
#endif
#ifdef USE_TEXT_SENSOR
  void set_client_ip_text_sensor(text_sensor::TextSensor *s) { this->client_ip_ts_ = s; }
#endif
#ifdef USE_SENSOR
  void set_bytes_sent_sensor(sensor::Sensor *s) { this->bytes_sent_sensor_ = s; }
  void set_cpu_use_pct_sensor(sensor::Sensor *s) { this->cpu_use_pct_sensor_ = s; }
  void set_peak_level_dbfs_sensor(sensor::Sensor *s) { this->peak_level_dbfs_sensor_ = s; }
  void set_limiter_gain_reduction_db_sensor(sensor::Sensor *s) { this->limiter_gain_reduction_db_sensor_ = s; }
#endif

 protected:
  // RTP payload type for dynamic L16 mapping in our SDP.
  static constexpr uint8_t RTP_PAYLOAD_TYPE = 96;
  static constexpr size_t RTP_HEADER_BYTES = 12;
  // RTSP TCP-interleaved framing prefix: '$' + 1-byte channel + 2-byte length.
  static constexpr size_t INTERLEAVE_HEADER_BYTES = 4;
  // Control-socket output buffer sizing. `tx_buffer_` is reserve()d to
  // TX_BUFFER_CAPACITY_BYTES once at setup() so it never reallocates — growing
  // a std::string toward a large size needs old+new buffers at once and can
  // exhaust the heap (and abort) on a no-PSRAM board. Interleaved RTP queueing
  // stops at MAX_RTP_BACKLOG_BYTES, leaving headroom for RTSP responses so the
  // buffer never has to grow past its reserved capacity.
  static constexpr size_t TX_BUFFER_CAPACITY_BYTES = 8192;
  static constexpr size_t MAX_RTP_BACKLOG_BYTES = 7168;

  // RTSP session inactivity timeout. Advertised verbatim in SETUP's
  // `Session: ...;timeout=` field and enforced locally so the two can't
  // drift.
  static constexpr uint32_t SESSION_TIMEOUT_SECONDS = 60;

  // Networking lifecycle.
  void start_listen_socket_();
  void try_accept_();
  void drain_control_socket_();
  void close_session_();
  /// Closes the session if no RTSP request has arrived within
  /// `session_timeout_seconds_`. Runs once per loop() tick.
  void check_session_inactivity_();

  // RTSP message dispatch.
  bool handle_rtsp_message_(const std::string &request);
  void send_rtsp_response_(const std::string &response);
  /// Pushes as much of `tx_buffer_` to the control socket as it will accept.
  /// All control-socket output (RTSP responses and interleaved RTP) flows
  /// through this one buffer so the byte stream stays correctly ordered.
  void flush_tx_buffer_();
  std::string build_sdp_() const;

  // Audio path.
  /// Allocates the ring buffer + RTP packet buffer the first time we go to
  /// PLAY. Both go through `RAMAllocator<uint8_t>` so capable boards can
  /// place them in PSRAM (the same pattern voice_assistant uses).
  bool allocate_stream_buffers_();
  void deallocate_stream_buffers_();
  void attach_mic_callback_();
  void start_streaming_();
  void stop_streaming_();
  void maybe_send_rtp_();
  /// Builds and sends exactly one RTP packet. Returns true only if a packet
  /// left the socket; false means "no audio buffered yet" or "socket busy",
  /// in which case the caller must not advance the pacing deadline.
  bool send_one_rtp_packet_();
  /// Emits the one-time first-packet confirmation, the >1 s underrun warning,
  /// and the periodic throughput line. Called every loop() while streaming.
  void log_stream_stats_(int64_t now);

  /// Pushes the current session_active_ / client_rtp_addr_ state to whichever
  /// of the optional client_connected / client_ip sensors the user wired up.
  /// Called at every session edge (SETUP success, close_session_).
  void publish_session_state_();

  // Configuration set by codegen / YAML.
  microphone::MicrophoneSource *mic_source_{nullptr};
  uint16_t listen_port_{8554};
  uint16_t packet_duration_ms_{20};

  // Cached audio shape for the active microphone source.
  audio::AudioStreamInfo stream_info_{};
  uint32_t samples_per_packet_{0};

  std::unique_ptr<::esphome::RingBuffer> ring_buffer_;

  // Sockets.
  std::unique_ptr<socket::Socket> listen_socket_;
  std::unique_ptr<socket::Socket> control_socket_;
  std::unique_ptr<socket::Socket> rtp_socket_;
  std::string rx_buffer_;

  // RTSP session state.
  bool session_active_{false};
  bool streaming_{false};
  // Deferred ring-buffer / RTP-packet free across the mic's asynchronous
  // stop. See teardown_guard.h for the full rationale.
  internal::TeardownGuard teardown_guard_;
  uint32_t session_id_{1};
  std::string content_base_;
  std::string track_url_;

  // Transport: false = RTP over UDP, true = RTP interleaved on the RTSP TCP
  // connection. Chosen per-client at SETUP. `tx_buffer_` holds pending
  // control-socket output for both RTSP responses and interleaved RTP.
  bool interleaved_{false};
  uint8_t rtp_channel_{0};
  std::string tx_buffer_;

  // RTP destination + bookkeeping.
  sockaddr_storage client_rtp_addr_{};
  uint16_t server_rtp_port_{0};
  uint16_t rtp_seq_{0};
  uint32_t rtp_ts_{0};
  uint32_t rtp_ssrc_{0};
  int64_t last_rtp_usec_{0};
  int64_t last_rtsp_activity_usec_{0};
  uint32_t rtp_interval_usec_{20'000};

  // Streaming diagnostics (all reset on each PLAY).
  uint32_t rtp_packets_sent_{0};
  uint32_t bytes_sent_{0};
  uint32_t stats_last_packets_{0};
  int64_t last_stats_usec_{0};
  int64_t last_packet_usec_{0};
  bool first_packet_logged_{false};
  bool underrun_warned_{false};

  // Microphone delivery diagnostics — counts what arrives from MicrophoneSource,
  // independent of the RTP send path, to pinpoint where the audio chain breaks.
  uint32_t mic_callbacks_{0};
  uint32_t mic_empty_callbacks_{0};
  uint32_t mic_bytes_received_{0};

  // DC blocker (1-pole HP at a fixed 5 Hz). Sits upstream of every
  // other DSP stage — always on, not user-configurable. Kills the
  // MEMS DC offset before it reaches the low-cut or gain stages.
  // Reset to zero at the start of each PLAY so a new session doesn't
  // inherit the previous one's transient.
  internal::DcBlockerState dc_blocker_state_{};

  // Low-cut filter (2nd-order Butterworth high-pass) state, applied per
  // sample in the RTP send loop. Reset to zero at the start of each
  // PLAY so a new session doesn't inherit the previous one's transient.
  // Coefficients and the source-of-truth frequency are held separately
  // because they survive across sessions and track the HA-controlled
  // value. `lowcut_bypass_` mirrors the highcut equivalent: any HA
  // slider value below LOW_CUT_MIN_CUTOFF_HZ (20 Hz) disables the
  // stage, leaving only the always-on DC blocker upstream.
  internal::BiquadState lowcut_state_{};
  internal::BiquadCoeffs lowcut_coeffs_{
      internal::low_cut_butterworth_coeffs(static_cast<float>(internal::LOW_CUT_DEFAULT_CUTOFF_HZ), 32000.0f)};
  bool lowcut_bypass_{false};
  float lowcut_filter_frequency_hz_{static_cast<float>(internal::LOW_CUT_DEFAULT_CUTOFF_HZ)};

  // High-cut filter (2nd-order Butterworth low-pass) state. Same
  // lifecycle as the low-cut: reset to zero at each PLAY so a new
  // session doesn't inherit the previous one's transient. Defaults to
  // the off sentinel (cutoff at Nyquist) so an un-touched HA install
  // streams bit-identical bytes.
  internal::BiquadState highcut_state_{};
  internal::BiquadCoeffs highcut_coeffs_{};
  bool highcut_bypass_{true};
  float highcut_filter_frequency_hz_{static_cast<float>(internal::HIGH_CUT_DEFAULT_CUTOFF_HZ)};

  // Software audio gain. Stored in Q8 so the RTP loop multiplies once
  // per sample; `internal::GAIN_Q8_UNITY` (256) is the bit-identical
  // skip-scaling fast path. Atomic so the number platform can update it
  // from the HA control callback without locking against the audio loop.
  std::atomic<int32_t> gain_q8_{internal::GAIN_Q8_UNITY};

  // Soft limiter (peak limiter with envelope follower). Sits after the
  // gain stage; bypass_ defaults to true (opt-in, disabled until the
  // HA switch is turned on). State is reset to zero at the start of
  // each PLAY so a new session starts with a clean envelope estimate.
  // Coefficients and threshold are recomputed whenever the HA sliders
  // move, not per sample.
  internal::SoftLimiterState soft_limiter_state_{};
  bool soft_limiter_bypass_{true};
  float soft_limiter_threshold_linear_{internal::limiter_db_to_linear(internal::SOFT_LIMITER_THRESHOLD_DB_DEFAULT)};
  float soft_limiter_attack_coeff_{internal::limiter_time_coeff(internal::SOFT_LIMITER_ATTACK_MS_DEFAULT, 32000.0f)};
  float soft_limiter_release_coeff_{internal::limiter_time_coeff(internal::SOFT_LIMITER_RELEASE_MS_DEFAULT, 32000.0f)};
  // Stored in original units for dump_config.
  float soft_limiter_threshold_db_{internal::SOFT_LIMITER_THRESHOLD_DB_DEFAULT};
  float soft_limiter_attack_ms_{internal::SOFT_LIMITER_ATTACK_MS_DEFAULT};
  float soft_limiter_release_ms_{internal::SOFT_LIMITER_RELEASE_MS_DEFAULT};

#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *client_connected_bs_{nullptr};
#endif
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *client_ip_ts_{nullptr};
  // Last value published, so we only push on change.
  std::string client_ip_published_;
#endif
#ifdef USE_SENSOR
  sensor::Sensor *bytes_sent_sensor_{nullptr};
  uint32_t bytes_sent_published_{UINT32_MAX};
  sensor::Sensor *cpu_use_pct_sensor_{nullptr};
  // Last published percentage as tenths-of-percent (0..1000), so we can publish
  // on change without floating-point comparisons. UINT16_MAX means "never
  // published"; 0 means "last publish was 0.0 %", which is a valid value.
  uint16_t cpu_use_published_tenths_{UINT16_MAX};
  // Post-gain peak meter. `window_peak_abs_` accumulates max |sample| seen
  // across all packets in the current 5 s stats window, then resets after
  // each publish. `peak_level_published_dbfs_` is the last value sent to HA
  // in whole dB; INT16_MAX is the "never published" sentinel so the first
  // value always emits even if it happens to land on the silence floor.
  // The silence floor (a real, in-band number) is also what we publish on
  // session close so HA's history graph stays a continuous numeric series
  // rather than introducing an unavailable gap.
  sensor::Sensor *peak_level_dbfs_sensor_{nullptr};
  uint16_t window_peak_abs_{0};
  int16_t peak_level_published_dbfs_{INT16_MAX};
  static constexpr int16_t SILENCE_FLOOR_DBFS = -100;
  // Soft limiter gain-reduction meter. Accumulates the per-packet minimum gain
  // (0..1 float) across the 5 s window; published as dB of reduction (0 = no
  // limiting, positive = limiting active). Mirrors the peak_level_dbfs pattern:
  // max reduction in the window, resets after each publish. INT16_MAX as the
  // "never published" sentinel so the first value always emits.
  sensor::Sensor *limiter_gain_reduction_db_sensor_{nullptr};
  float window_min_sl_gain_{1.0f};
  int16_t limiter_gr_published_db_{INT16_MAX};
#endif

  // CPU-use self-instrumentation. `busy_usec_` accumulates the wall-clock µs
  // spent inside `loop()` and the mic data callback; `cpu_window_start_usec_`
  // marks the start of the current measurement window so the percentage is
  // (busy / (now - window_start)) * 100. The atomic exists because the mic
  // callback can fire from the I²S driver task on dual-core builds; relaxed
  // ordering is enough since we only need eventual visibility, not a happens-
  // before edge with any other data. The reported value covers our own work
  // only — Wi-Fi/LwIP, the I²S driver, and other ESPHome components are not
  // counted (see CHANGELOG / docs for how to read the number).
  std::atomic<int64_t> busy_usec_{0};
  int64_t cpu_window_start_usec_{0};
  // Counts 5 s stats ticks so we can publish CPU-use every other tick (~10 s)
  // without adding a second timer. Reset on each PLAY.
  uint8_t stats_tick_{0};

  // RTP packet buffer (header + payload). Allocated via RAMAllocator on PLAY,
  // freed on TEARDOWN. `rtp_packet_size_` is computed at setup() so we know
  // up-front how much we'll need to allocate later.
  uint8_t *rtp_packet_{nullptr};
  size_t rtp_packet_size_{0};
};

}  // namespace esphome::rtsp_audio

#endif  // USE_RTSP_AUDIO
