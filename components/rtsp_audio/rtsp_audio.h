#pragma once

#include "esphome/core/defines.h"
#ifdef USE_RTSP_AUDIO

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "esphome/components/audio/audio.h"
#include "esphome/components/microphone/microphone_source.h"
#include "esphome/components/socket/socket.h"
#include "esphome/core/component.h"
#include "esphome/core/ring_buffer.h"

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

  // Networking lifecycle.
  void start_listen_socket_();
  void try_accept_();
  void drain_control_socket_();
  void close_session_();

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
  uint32_t rtp_interval_usec_{20'000};

  // Streaming diagnostics (all reset on each PLAY).
  uint32_t rtp_packets_sent_{0};
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

  // RTP packet buffer (header + payload). Allocated via RAMAllocator on PLAY,
  // freed on TEARDOWN. `rtp_packet_size_` is computed at setup() so we know
  // up-front how much we'll need to allocate later.
  uint8_t *rtp_packet_{nullptr};
  size_t rtp_packet_size_{0};
};

}  // namespace esphome::rtsp_audio

#endif  // USE_RTSP_AUDIO
