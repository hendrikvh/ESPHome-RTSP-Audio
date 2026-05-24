#pragma once

#include "esphome/core/defines.h"
#ifdef USE_RTSP_AUDIO

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "dc_blocker.h"
#include "gain.h"
#include "high_cut.h"
#include "esphome/components/audio/audio.h"
#include "esphome/components/microphone/microphone_source.h"
#include "esphome/components/socket/socket.h"
#include "esphome/core/component.h"
#include "esphome/core/ring_buffer.h"

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
  /// Out-of-range values are clamped to [DC_BLOCKER_MIN_CUTOFF_HZ,
  /// DC_BLOCKER_MAX_CUTOFF_HZ].
  void set_lowcut_filter_frequency_hz(float hz);

  /// Updates the high-cut filter frequency (in Hz) at runtime. Called
  /// from the bundled `number` platform when the HA slider moves and
  /// from `Number::setup()` when the persisted value is restored on
  /// boot. Out-of-range values are clamped to
  /// [HIGH_CUT_MIN_CUTOFF_HZ, HIGH_CUT_MAX_CUTOFF_HZ]; a cutoff at the
  /// max disables the filter via the bit-identical fast path in the
  /// per-sample loop.
  void set_highcut_filter_frequency_hz(float hz);

  /// Updates the software input gain (in dB) at runtime. Called from
  /// the bundled `number` platform on slider moves and on restore.
  /// Out-of-range values are clamped to
  /// [internal::GAIN_DB_MIN, internal::GAIN_DB_MAX]; 0 dB takes the
  /// bit-identical fast path in the per-sample loop. Internally the
  /// dB value is converted to a Q8 linear coefficient — the audio hot
  /// path never sees dB.
  void set_gain_db(float db);

#ifdef USE_BINARY_SENSOR
  void set_client_connected_binary_sensor(binary_sensor::BinarySensor *s) { this->client_connected_bs_ = s; }
#endif
#ifdef USE_TEXT_SENSOR
  void set_client_ip_text_sensor(text_sensor::TextSensor *s) { this->client_ip_ts_ = s; }
#endif
#ifdef USE_SENSOR
  void set_bytes_sent_sensor(sensor::Sensor *s) { this->bytes_sent_sensor_ = s; }
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

  // Low-cut filter (one-pole IIR high-pass) state, applied per sample in
  // the RTP send loop. Reset to zero at the start of each PLAY so a new
  // session doesn't inherit the previous one's transient. The Q15
  // coefficient and the source-of-truth frequency are held separately
  // because they survive across sessions and track the HA-controlled
  // value.
  internal::DcBlockerState dc_blocker_state_{};
  int32_t lowcut_filter_r_q15_{internal::DC_BLOCKER_DEFAULT_R_Q15};
  float lowcut_filter_frequency_hz_{static_cast<float>(internal::DC_BLOCKER_DEFAULT_CUTOFF_HZ)};

  // High-cut filter (one-pole IIR low-pass) state. Same lifecycle as the
  // low-cut: reset to zero at each PLAY so a new session doesn't
  // inherit the previous one's transient. Defaults to the off sentinel
  // so an un-touched HA install streams bit-identical bytes.
  internal::HighCutState high_cut_state_{};
  int32_t highcut_filter_a_q15_{internal::HIGH_CUT_DEFAULT_A_Q15};
  float highcut_filter_frequency_hz_{static_cast<float>(internal::HIGH_CUT_DEFAULT_CUTOFF_HZ)};

  // Software input gain. Stored in Q8 so the RTP loop multiplies once
  // per sample; `internal::GAIN_Q8_UNITY` (256) is the bit-identical
  // skip-scaling fast path. Atomic so the number platform can update it
  // from the HA control callback without locking against the audio loop.
  std::atomic<int32_t> gain_q8_{internal::GAIN_Q8_UNITY};

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
#endif

  // RTP packet buffer (header + payload). Allocated via RAMAllocator on PLAY,
  // freed on TEARDOWN. `rtp_packet_size_` is computed at setup() so we know
  // up-front how much we'll need to allocate later.
  uint8_t *rtp_packet_{nullptr};
  size_t rtp_packet_size_{0};
};

}  // namespace esphome::rtsp_audio

#endif  // USE_RTSP_AUDIO
