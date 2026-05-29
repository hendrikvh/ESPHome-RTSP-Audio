#include "rtsp_audio.h"

#include "audio_pipeline.h"
#include "session_timeout.h"
#ifdef USE_RTSP_AUDIO

#include <arpa/inet.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <strings.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "esphome/components/network/util.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome::rtsp_audio {

static const char *const TAG = "rtsp_audio";

// Adds (esp_timer_get_time() - start) µs to a target atomic on scope exit.
// Used to self-time the rtsp_audio main-loop body and the mic data callback
// for the cpu_use_pct sensor. Cost per scope: two esp_timer reads and one
// relaxed atomic add — well under 0.1 % of CPU at the cadences these scopes
// run at, so the measurement itself does not meaningfully bias the result it
// reports.
class BusyScope {
 public:
  explicit BusyScope(std::atomic<int64_t> &accumulator) : accumulator_(accumulator), start_(esp_timer_get_time()) {}
  ~BusyScope() {
    const int64_t elapsed = esp_timer_get_time() - this->start_;
    if (elapsed > 0)
      this->accumulator_.fetch_add(elapsed, std::memory_order_relaxed);
  }
  BusyScope(const BusyScope &) = delete;
  BusyScope &operator=(const BusyScope &) = delete;

 private:
  std::atomic<int64_t> &accumulator_;
  int64_t start_;
};

// Helpers that don't need access to the component instance live here so the
// class surface stays small.
namespace {

bool header_starts_with(const std::string &line, const char *name) {
  size_t n = std::strlen(name);
  return line.size() >= n && strncasecmp(line.c_str(), name, n) == 0;
}

std::string parse_request_uri(const std::string &request_line) {
  auto sp1 = request_line.find(' ');
  if (sp1 == std::string::npos)
    return {};
  auto sp2 = request_line.find(' ', sp1 + 1);
  if (sp2 == std::string::npos)
    return {};
  return request_line.substr(sp1 + 1, sp2 - sp1 - 1);
}

struct ParsedTransport {
  bool valid{false};
  bool interleaved{false};  // true = RTP/AVP/TCP framed on the RTSP connection
  uint16_t client_rtp_port{0};
  uint16_t client_rtcp_port{0};
  uint8_t rtp_channel{0};
  uint8_t rtcp_channel{1};
};

ParsedTransport parse_transport(const std::string &header) {
  ParsedTransport out;
  auto colon = header.find(':');
  if (colon == std::string::npos)
    return out;
  std::string value = header.substr(colon + 1);

  // TCP-interleaved transport: RTP is framed on the RTSP connection itself
  // (RFC 2326 section 10.12). FFmpeg, NVRs, and most non-VLC clients default
  // to this. `interleaved=<rtp>-<rtcp>` names the two channel ids.
  if (value.find("RTP/AVP/TCP") != std::string::npos || value.find("interleaved") != std::string::npos) {
    out.interleaved = true;
    const char *ic = std::strstr(value.c_str(), "interleaved=");
    unsigned c1 = 0;
    unsigned c2 = 0;
    if (ic != nullptr && std::sscanf(ic, "interleaved=%u-%u", &c1, &c2) == 2) {
      out.rtp_channel = static_cast<uint8_t>(c1);
      out.rtcp_channel = static_cast<uint8_t>(c2);
    } else if (ic != nullptr && std::sscanf(ic, "interleaved=%u", &c1) == 1) {
      out.rtp_channel = static_cast<uint8_t>(c1);
      out.rtcp_channel = static_cast<uint8_t>(c1 + 1);
    }
    out.valid = true;
    return out;
  }

  // Plain UDP RTP/AVP: the client advertises its receive ports via client_port.
  const char *kp = std::strstr(value.c_str(), "client_port=");
  if (kp == nullptr)
    return out;

  unsigned p1 = 0;
  unsigned p2 = 0;
  if (std::sscanf(kp, "client_port=%u-%u", &p1, &p2) == 2) {
    // pair
  } else if (std::sscanf(kp, "client_port=%u", &p1) == 1) {
    p2 = p1 + 1;
  } else {
    return out;
  }

  out.client_rtp_port = static_cast<uint16_t>(p1);
  out.client_rtcp_port = static_cast<uint16_t>(p2);
  out.valid = true;
  return out;
}

std::vector<std::string> split_crlf(const std::string &message) {
  std::vector<std::string> lines;
  size_t cursor = 0;
  while (cursor < message.size()) {
    auto end = message.find("\r\n", cursor);
    if (end == std::string::npos) {
      lines.emplace_back(message.substr(cursor));
      break;
    }
    lines.emplace_back(message.substr(cursor, end - cursor));
    cursor = end + 2;
  }
  return lines;
}

}  // namespace

void RtspAudioComponent::setup() {
  if (this->mic_source_ == nullptr) {
    ESP_LOGE(TAG, "No microphone source configured");
    this->mark_failed();
    return;
  }

  this->stream_info_ = this->mic_source_->get_audio_stream_info();
  // FINAL_VALIDATE_SCHEMA already fixes this to 32 kHz mono 16-bit, but a runtime
  // guard keeps the audio math honest if someone bypasses validation.
  if (this->stream_info_.get_sample_rate() != 32000 || this->stream_info_.get_channels() != 1 ||
      this->stream_info_.get_bits_per_sample() != 16) {
    ESP_LOGE(TAG, "Unsupported microphone stream: %u Hz / %u ch / %u bit", this->stream_info_.get_sample_rate(),
             this->stream_info_.get_channels(), this->stream_info_.get_bits_per_sample());
    this->mark_failed();
    return;
  }

  this->samples_per_packet_ = this->stream_info_.ms_to_samples(this->packet_duration_ms_);
  if (this->samples_per_packet_ == 0) {
    ESP_LOGE(TAG, "packet_ms %u is too small for sample rate", this->packet_duration_ms_);
    this->mark_failed();
    return;
  }
  // Use AudioStreamInfo math so pacing stays accurate even at non-integer-ms rates (e.g. 44.1 kHz).
  // For mono audio one frame == one sample, so the call below matches the per-packet duration exactly.
  this->rtp_interval_usec_ = this->stream_info_.frames_to_microseconds(this->samples_per_packet_);
  this->rtp_packet_size_ = RTP_HEADER_BYTES + this->stream_info_.samples_to_bytes(this->samples_per_packet_);

  this->attach_mic_callback_();

  // Reserve the control-socket output buffer once, up front, so it never has to
  // reallocate later. A large std::string reallocation needs the old and new
  // buffers simultaneously and can abort on a low-RAM board.
  this->tx_buffer_.reserve(TX_BUFFER_CAPACITY_BYTES);

  this->start_listen_socket_();
}

void RtspAudioComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "RTSP audio:");
  ESP_LOGCONFIG(TAG, "  Listen port: %u", this->listen_port_);
  ESP_LOGCONFIG(TAG, "  Packet ms: %u", this->packet_duration_ms_);
  ESP_LOGCONFIG(TAG, "  Session timeout: %us", SESSION_TIMEOUT_SECONDS);
  ESP_LOGCONFIG(TAG, "  Audio: %u Hz / %u ch / %u bit, %u samples/pkt", this->stream_info_.get_sample_rate(),
                this->stream_info_.get_channels(), this->stream_info_.get_bits_per_sample(), this->samples_per_packet_);
  ESP_LOGCONFIG(TAG, "  DC blocker: on (5 Hz, always)");
  if (this->lowcut_bypass_) {
    ESP_LOGCONFIG(TAG, "  Low-cut filter: off (DC blocker still active)");
  } else {
    ESP_LOGCONFIG(TAG, "  Low-cut filter frequency: %.1f Hz", this->lowcut_filter_frequency_hz_);
  }
  if (this->highcut_bypass_) {
    ESP_LOGCONFIG(TAG, "  High-cut filter: off");
  } else {
    ESP_LOGCONFIG(TAG, "  High-cut filter frequency: %.1f Hz", this->highcut_filter_frequency_hz_);
  }
  {
    const int32_t q8 = this->gain_q8_.load(std::memory_order_relaxed);
    const float linear = q8 / 256.0f;
    ESP_LOGCONFIG(TAG, "  Audio gain: %+.1f dB (%.2fx, Q8=%d)", internal::linear_to_db(linear), linear, q8);
  }
  if (this->soft_limiter_bypass_) {
    ESP_LOGCONFIG(TAG, "  Soft limiter: off");
  } else {
    ESP_LOGCONFIG(TAG, "  Soft limiter: on, threshold=%.1f dBFS, attack=%.1f ms, release=%.1f ms",
                  this->soft_limiter_threshold_db_, this->soft_limiter_attack_ms_, this->soft_limiter_release_ms_);
  }
#ifdef USE_BINARY_SENSOR
  LOG_BINARY_SENSOR("  ", "Client Connected", this->client_connected_bs_);
#endif
#ifdef USE_TEXT_SENSOR
  LOG_TEXT_SENSOR("  ", "Client IP", this->client_ip_ts_);
#endif
#ifdef USE_SENSOR
  LOG_SENSOR("  ", "Bytes Sent", this->bytes_sent_sensor_);
#endif
}

void RtspAudioComponent::set_low_cut_frequency_hz(float hz) {
  // Slider can drag below LOW_CUT_MIN_CUTOFF_HZ for the "off" sentinel,
  // and above the active range gets clamped. Symmetric with the
  // high-cut: bypass at the slider's edge, no in-between weirdness.
  const float clamped = std::clamp(hz, 0.0f, static_cast<float>(internal::LOW_CUT_MAX_CUTOFF_HZ));
  // Fall back to 32 kHz before stream_info_ is populated (e.g. when the
  // number entity's restore_value path fires during setup, before the
  // first SETUP latches the mic shape). The audio source is constrained
  // to 32 kHz anyway, so this matches the eventual runtime value.
  const float sr =
      this->stream_info_.get_sample_rate() != 0 ? static_cast<float>(this->stream_info_.get_sample_rate()) : 32000.0f;
  this->lowcut_filter_frequency_hz_ = clamped;
  this->lowcut_bypass_ = internal::low_cut_is_bypass(clamped);
  if (this->lowcut_bypass_) {
    this->lowcut_coeffs_ = {};
    ESP_LOGD(TAG, "Low-cut filter off (cutoff %.1f Hz; DC blocker still active)", clamped);
  } else {
    this->lowcut_coeffs_ = internal::low_cut_butterworth_coeffs(clamped, sr);
    ESP_LOGD(TAG, "Low-cut filter frequency set to %.1f Hz (Butterworth 2nd order)", clamped);
  }
}

void RtspAudioComponent::set_high_cut_frequency_hz(float hz) {
  const float clamped = std::clamp(hz, static_cast<float>(internal::HIGH_CUT_MIN_CUTOFF_HZ),
                                   static_cast<float>(internal::HIGH_CUT_MAX_CUTOFF_HZ));
  // Same stream_info_ fallback as the low-cut: the number entity's
  // restore_value path can fire during setup, before the first SETUP
  // latches the mic shape. Our audio source is constrained to 32 kHz
  // anyway, so this matches the eventual runtime value.
  const float sr =
      this->stream_info_.get_sample_rate() != 0 ? static_cast<float>(this->stream_info_.get_sample_rate()) : 32000.0f;
  this->highcut_filter_frequency_hz_ = clamped;
  this->highcut_bypass_ = internal::high_cut_is_bypass(clamped);
  if (this->highcut_bypass_) {
    this->highcut_coeffs_ = {};
    ESP_LOGD(TAG, "High-cut filter off (cutoff %.1f Hz)", clamped);
  } else {
    this->highcut_coeffs_ = internal::high_cut_butterworth_coeffs(clamped, sr);
    ESP_LOGD(TAG, "High-cut filter frequency set to %.1f Hz (Butterworth 2nd order)", clamped);
  }
}

void RtspAudioComponent::set_gain_db(float db) {
  const int32_t q8 = internal::gain_q8_for_db(db);
  this->gain_q8_.store(q8, std::memory_order_relaxed);
  const float linear = q8 / 256.0f;
  ESP_LOGD(TAG, "Audio gain set to %+.1f dB (%.2fx, Q8=%d)", db, linear, q8);
}

void RtspAudioComponent::set_soft_limiter_enabled(bool enabled) {
  this->soft_limiter_bypass_ = !enabled;
  ESP_LOGD(TAG, "Soft limiter %s", enabled ? "enabled" : "disabled");
}

void RtspAudioComponent::set_soft_limiter_threshold_db(float db) {
  const float clamped =
      std::clamp(db, internal::SOFT_LIMITER_THRESHOLD_DB_MIN, internal::SOFT_LIMITER_THRESHOLD_DB_MAX);
  this->soft_limiter_threshold_db_ = clamped;
  this->soft_limiter_threshold_linear_ = internal::limiter_db_to_linear(clamped);
  ESP_LOGD(TAG, "Soft limiter threshold set to %.1f dBFS (%.4f linear)", clamped, this->soft_limiter_threshold_linear_);
}

void RtspAudioComponent::set_soft_limiter_attack_ms(float ms) {
  const float clamped = std::clamp(ms, internal::SOFT_LIMITER_ATTACK_MS_MIN, internal::SOFT_LIMITER_ATTACK_MS_MAX);
  this->soft_limiter_attack_ms_ = clamped;
  this->soft_limiter_attack_coeff_ = internal::limiter_time_coeff(clamped, 32000.0f);
  ESP_LOGD(TAG, "Soft limiter attack set to %.1f ms (coeff=%.6f)", clamped, this->soft_limiter_attack_coeff_);
}

void RtspAudioComponent::set_soft_limiter_release_ms(float ms) {
  const float clamped = std::clamp(ms, internal::SOFT_LIMITER_RELEASE_MS_MIN, internal::SOFT_LIMITER_RELEASE_MS_MAX);
  this->soft_limiter_release_ms_ = clamped;
  this->soft_limiter_release_coeff_ = internal::limiter_time_coeff(clamped, 32000.0f);
  ESP_LOGD(TAG, "Soft limiter release set to %.1f ms (coeff=%.6f)", clamped, this->soft_limiter_release_coeff_);
}

void RtspAudioComponent::loop() {
  // RAII guard wraps the entire loop body so every exit path (including the
  // network-down early return) contributes to the cpu_use_pct accumulator.
  BusyScope busy{this->busy_usec_};
  if (!network::is_connected() && this->control_socket_) {
    ESP_LOGW(TAG, "Network down; closing RTSP session");
    this->close_session_();
    return;
  }
  if (this->listen_socket_ && !this->control_socket_)
    this->try_accept_();
  if (this->control_socket_)
    this->drain_control_socket_();
  if (this->control_socket_)
    this->check_session_inactivity_();
  this->maybe_send_rtp_();
  this->flush_tx_buffer_();
  if (this->mic_source_ != nullptr && this->teardown_guard_.poll(this->mic_source_->is_stopped())) {
    this->deallocate_stream_buffers_();
  }
}

void RtspAudioComponent::attach_mic_callback_() {
  this->mic_source_->add_data_callback([this](const std::vector<uint8_t> &data) {
    // RAII guard accumulates the callback's wall-clock cost into the same
    // busy_usec_ as loop(). The mic callback runs from the I²S driver task,
    // which is why busy_usec_ is std::atomic.
    BusyScope busy{this->busy_usec_};
    // Diagnostics: record what MicrophoneSource actually hands us, separate from
    // the RTP send path. Empty buffers mean the I2S layer read no audio.
    this->mic_callbacks_++;
    if (data.empty()) {
      this->mic_empty_callbacks_++;
    } else {
      this->mic_bytes_received_ += data.size();
    }
    if (this->ring_buffer_ != nullptr)
      this->ring_buffer_->write(data.data(), data.size());
  });
}

bool RtspAudioComponent::allocate_stream_buffers_() {
  // Rapid reconnect: a previous close_session_() may have left the dealloc
  // pending while the mic task was still draining. Reuse the existing
  // buffers and cancel the deferred free so loop() doesn't pull them out
  // from under the new session.
  this->teardown_guard_.cancel();
  // RingBuffer::create() already uses RAMAllocator<uint8_t> internally, so
  // ~1 s of jitter slack lands in PSRAM on capable boards automatically.
  // At 32 kHz mono 16-bit that's 64 KB — fits in internal RAM on no-PSRAM
  // boards (2 s would be 128 KB and routinely fails to allocate).
  if (this->ring_buffer_ == nullptr) {
    const size_t bytes = this->stream_info_.ms_to_bytes(1000);
    this->ring_buffer_ = ::esphome::RingBuffer::create(bytes);
    if (this->ring_buffer_ == nullptr) {
      ESP_LOGE(TAG, "Ring buffer allocate failed (%zu bytes)", bytes);
      return false;
    }
  }
  // Reusable RTP packet buffer. Default RAMAllocator prefers SPIRAM and
  // falls back to internal heap, matching voice_assistant's pattern.
  if (this->rtp_packet_ == nullptr) {
    RAMAllocator<uint8_t> allocator;
    this->rtp_packet_ = allocator.allocate(this->rtp_packet_size_);
    if (this->rtp_packet_ == nullptr) {
      ESP_LOGE(TAG, "RTP packet buffer allocate failed (%zu bytes)", this->rtp_packet_size_);
      return false;
    }
  }
  return true;
}

void RtspAudioComponent::deallocate_stream_buffers_() {
  this->ring_buffer_.reset();
  if (this->rtp_packet_ != nullptr) {
    RAMAllocator<uint8_t> allocator;
    allocator.deallocate(this->rtp_packet_, this->rtp_packet_size_);
    this->rtp_packet_ = nullptr;
  }
}

void RtspAudioComponent::start_listen_socket_() {
  this->listen_socket_ = socket::socket_ip_loop_monitored(SOCK_STREAM, IPPROTO_TCP);
  if (this->listen_socket_ == nullptr) {
    ESP_LOGE(TAG, "Listen socket alloc failed");
    this->mark_failed();
    return;
  }

  int enable = 1;
  if (this->listen_socket_->setsockopt(SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) != 0)
    ESP_LOGW(TAG, "listen SO_REUSEADDR failed errno=%d", errno);

  if (this->listen_socket_->setblocking(false) != 0) {
    ESP_LOGE(TAG, "listen setblocking failed errno=%d", errno);
    this->mark_failed();
    return;
  }

  sockaddr_storage server{};
  socklen_t slen = socket::set_sockaddr_any(reinterpret_cast<sockaddr *>(&server), sizeof(server), this->listen_port_);
  if (slen == 0) {
    ESP_LOGE(TAG, "set_sockaddr_any failed errno=%d", errno);
    this->mark_failed();
    return;
  }

  if (this->listen_socket_->bind(reinterpret_cast<sockaddr *>(&server), slen) != 0) {
    ESP_LOGE(TAG, "bind port %u errno=%d", this->listen_port_, errno);
    this->mark_failed();
    return;
  }

  if (this->listen_socket_->listen(1) != 0) {
    ESP_LOGE(TAG, "listen() errno=%d", errno);
    this->mark_failed();
    return;
  }

  ESP_LOGI(TAG, "RTSP listening on port %u (L16/%u/1, PT %u)", this->listen_port_, this->stream_info_.get_sample_rate(),
           RTP_PAYLOAD_TYPE);
}

void RtspAudioComponent::try_accept_() {
  if (!this->listen_socket_->ready())
    return;

  sockaddr_storage remote{};
  socklen_t rlen = sizeof(remote);
  auto cli = this->listen_socket_->accept_loop_monitored(reinterpret_cast<sockaddr *>(&remote), &rlen);
  if (!cli)
    return;

  cli->setblocking(false);

  if (this->control_socket_) {
    ESP_LOGW(TAG, "Reject second RTSP client (single session MVP)");
    cli->shutdown(SHUT_RDWR);
    return;
  }

  this->control_socket_ = std::move(cli);
  this->session_id_ = esp_random() | 1U;
  this->rx_buffer_.clear();
  this->session_active_ = false;
  this->streaming_ = false;
  this->content_base_.clear();
  this->track_url_.clear();
  this->interleaved_ = false;
  this->tx_buffer_.clear();
  this->last_rtsp_activity_usec_ = esp_timer_get_time();
  ESP_LOGI(TAG, "RTSP client accepted (session %u)", this->session_id_);
}

void RtspAudioComponent::drain_control_socket_() {
  if (!this->control_socket_->ready())
    return;

  uint8_t scratch[384];
  ssize_t r = this->control_socket_->read(scratch, sizeof(scratch));
  if (r == 0) {
    ESP_LOGI(TAG, "RTSP peer closed");
    this->close_session_();
    return;
  }
  if (r < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return;
    ESP_LOGW(TAG, "RTSP read errno=%d", errno);
    this->close_session_();
    return;
  }

  this->rx_buffer_.append(reinterpret_cast<const char *>(scratch), static_cast<size_t>(r));

  while (!this->rx_buffer_.empty()) {
    // An interleaved binary frame from the client (e.g. an RTCP receiver
    // report) starts with '$'. We don't consume RTCP, so skip the whole frame
    // rather than letting binary bytes corrupt RTSP request parsing.
    if (static_cast<uint8_t>(this->rx_buffer_[0]) == '$') {
      if (this->rx_buffer_.size() < INTERLEAVE_HEADER_BYTES)
        break;  // wait for the full 4-byte framing header
      const size_t frame_len =
          (static_cast<uint8_t>(this->rx_buffer_[2]) << 8) | static_cast<uint8_t>(this->rx_buffer_[3]);
      if (this->rx_buffer_.size() < INTERLEAVE_HEADER_BYTES + frame_len)
        break;  // wait for the full frame
      this->rx_buffer_.erase(0, INTERLEAVE_HEADER_BYTES + frame_len);
      continue;
    }

    auto end = this->rx_buffer_.find("\r\n\r\n");
    if (end == std::string::npos)
      break;
    std::string message = this->rx_buffer_.substr(0, end + 4);
    this->rx_buffer_.erase(0, end + 4);
    if (!this->handle_rtsp_message_(message))
      return;
  }
}

void RtspAudioComponent::send_rtsp_response_(const std::string &response) {
  if (!this->control_socket_)
    return;
  // Queue, don't write directly: RTSP responses and interleaved RTP share the
  // control socket and must stay correctly ordered on the wire.
  this->tx_buffer_.append(response);
  this->flush_tx_buffer_();
}

void RtspAudioComponent::flush_tx_buffer_() {
  if (this->tx_buffer_.empty() || !this->control_socket_)
    return;
  ssize_t wr = this->control_socket_->write(this->tx_buffer_.data(), this->tx_buffer_.size());
  if (wr < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return;  // socket full; retry next loop
    ESP_LOGW(TAG, "Control socket write errno=%d", errno);
    this->close_session_();
    return;
  }
  if (wr > 0)
    this->tx_buffer_.erase(0, static_cast<size_t>(wr));
}

std::string RtspAudioComponent::build_sdp_() const {
  std::string sdp = str_sprintf(
      "v=0\r\n"
      "o=- 0 0 IN IP4 0.0.0.0\r\n"
      "s=ESPHome RTSP microphone\r\n"
      "i=live L16 PCM\r\n"
      "t=0 0\r\n"
      "m=audio 0 RTP/AVP %u\r\n"
      "c=IN IP4 0.0.0.0\r\n"
      "a=rtpmap:%u L16/%u/1\r\n",
      static_cast<unsigned>(RTP_PAYLOAD_TYPE), static_cast<unsigned>(RTP_PAYLOAD_TYPE),
      this->stream_info_.get_sample_rate());
  if (!this->track_url_.empty())
    sdp += "a=control:" + this->track_url_ + "\r\n";
  return sdp;
}

bool RtspAudioComponent::handle_rtsp_message_(const std::string &request) {
  auto lines = split_crlf(request);
  if (lines.empty())
    return true;

  this->last_rtsp_activity_usec_ = esp_timer_get_time();

  const std::string &request_line = lines[0];
  auto sp_method = request_line.find(' ');
  if (sp_method == std::string::npos)
    return true;
  const std::string method = str_lower_case(request_line.substr(0, sp_method));

  std::string cseq_hdr = "CSeq: 0\r\n";
  std::string transport_hdr;
  for (size_t i = 1; i < lines.size(); i++) {
    const std::string &line = lines[i];
    if (header_starts_with(line, "CSeq")) {
      cseq_hdr = line + "\r\n";
    } else if (header_starts_with(line, "Transport")) {
      transport_hdr = line;
    }
  }

  if (method == "options") {
    this->send_rtsp_response_(str_sprintf(
        "RTSP/1.0 200 OK\r\n%sPublic: OPTIONS, DESCRIBE, SETUP, TEARDOWN, PLAY, PAUSE, GET_PARAMETER\r\n\r\n",
        cseq_hdr.c_str()));
    return true;
  }

  if (method == "get_parameter") {
    this->send_rtsp_response_(str_sprintf("RTSP/1.0 200 OK\r\n%s\r\n", cseq_hdr.c_str()));
    return true;
  }

  if (method == "describe") {
    std::string uri = parse_request_uri(request_line);
    if (!uri.empty())
      this->content_base_ = uri;

    this->track_url_ = this->content_base_;
    if (!this->track_url_.empty()) {
      if (this->track_url_.back() == '/')
        this->track_url_.pop_back();
      if (this->track_url_.find("track1") == std::string::npos)
        this->track_url_ += "/track1";
    }

    std::string sdp = this->build_sdp_();
    this->send_rtsp_response_(str_sprintf(
        "RTSP/1.0 200 OK\r\n%sContent-Type: application/sdp\r\nContent-Base: %s\r\nContent-Length: %zu\r\n\r\n%s",
        cseq_hdr.c_str(), this->content_base_.c_str(), sdp.size(), sdp.c_str()));
    return true;
  }

  if (method == "setup") {
    ESP_LOGD(TAG, "SETUP transport request: \"%s\"", transport_hdr.c_str());
    ParsedTransport tr = parse_transport(transport_hdr);
    if (!tr.valid) {
      ESP_LOGW(TAG,
               "Rejecting SETUP 461: need RTP/AVP UDP with client_port= or RTP/AVP/TCP interleaved. "
               "Client requested: \"%s\"",
               transport_hdr.c_str());
      this->send_rtsp_response_(str_sprintf("RTSP/1.0 461 Unsupported Transport\r\n%s\r\n", cseq_hdr.c_str()));
      return true;
    }

    if (tr.interleaved) {
      // RTP framed on the RTSP TCP connection — no separate UDP socket needed.
      this->interleaved_ = true;
      this->rtp_channel_ = tr.rtp_channel;
      this->rtp_socket_.reset();
      this->send_rtsp_response_(
          str_sprintf("RTSP/1.0 200 OK\r\n%sSession: %u;timeout=%u\r\n"
                      "Transport: RTP/AVP/TCP;unicast;interleaved=%u-%u\r\n\r\n",
                      cseq_hdr.c_str(), this->session_id_, SESSION_TIMEOUT_SECONDS,
                      static_cast<unsigned>(tr.rtp_channel), static_cast<unsigned>(tr.rtcp_channel)));
      this->session_active_ = true;
      this->publish_session_state_();
      ESP_LOGI(TAG, "SETUP: TCP-interleaved transport (RTP channel %u)", static_cast<unsigned>(tr.rtp_channel));
      return true;
    }

    // UDP transport: learn the client's RTP port and open our send socket.
    this->interleaved_ = false;
    sockaddr_storage peer{};
    socklen_t peer_len = sizeof(peer);
    if (this->control_socket_->getpeername(reinterpret_cast<sockaddr *>(&peer), &peer_len) != 0) {
      ESP_LOGE(TAG, "getpeername errno=%d", errno);
      this->close_session_();
      return false;
    }
    if (peer.ss_family != AF_INET) {
      ESP_LOGW(TAG,
               "Rejecting SETUP 461: UDP transport needs an IPv4 client; "
               "an IPv6 client can use TCP-interleaved (RTP/AVP/TCP) instead");
      this->send_rtsp_response_(str_sprintf("RTSP/1.0 461 Unsupported Transport\r\n%s\r\n", cseq_hdr.c_str()));
      return true;
    }

    this->client_rtp_addr_ = peer;
    reinterpret_cast<sockaddr_in *>(&this->client_rtp_addr_)->sin_port = htons(tr.client_rtp_port);

    this->rtp_socket_ = socket::socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (!this->rtp_socket_) {
      ESP_LOGE(TAG, "RTP UDP socket alloc failed");
      this->close_session_();
      return false;
    }
    if (this->rtp_socket_->setblocking(false) != 0) {
      ESP_LOGE(TAG, "RTP nonblocking errno=%d", errno);
      this->close_session_();
      return false;
    }

    sockaddr_storage rtp_bind{};
    socklen_t bl = socket::set_sockaddr_any(reinterpret_cast<sockaddr *>(&rtp_bind), sizeof(rtp_bind), 0);
    if (bl == 0 || this->rtp_socket_->bind(reinterpret_cast<sockaddr *>(&rtp_bind), bl) != 0) {
      ESP_LOGE(TAG, "RTP bind errno=%d", errno);
      this->close_session_();
      return false;
    }

    sockaddr_storage local{};
    socklen_t ll = sizeof(local);
    if (this->rtp_socket_->getsockname(reinterpret_cast<sockaddr *>(&local), &ll) == 0 && local.ss_family == AF_INET)
      this->server_rtp_port_ = ntohs(reinterpret_cast<sockaddr_in *>(&local)->sin_port);

    this->send_rtsp_response_(
        str_sprintf("RTSP/1.0 200 OK\r\n%sSession: %u;timeout=%u\r\n"
                    "Transport: RTP/AVP;unicast;client_port=%u-%u;server_port=%u-%u\r\n\r\n",
                    cseq_hdr.c_str(), this->session_id_, SESSION_TIMEOUT_SECONDS, tr.client_rtp_port,
                    tr.client_rtcp_port, this->server_rtp_port_, this->server_rtp_port_ + 1));
    this->session_active_ = true;
    this->publish_session_state_();
    return true;
  }

  if (method == "play") {
    if (!this->session_active_ || (!this->interleaved_ && !this->rtp_socket_)) {
      this->send_rtsp_response_(str_sprintf("RTSP/1.0 454 Session Not Found\r\n%s\r\n", cseq_hdr.c_str()));
      return true;
    }
    this->start_streaming_();

    std::string rtp_info;
    if (!this->track_url_.empty()) {
      rtp_info = str_sprintf("RTP-Info: url=%s;seq=%u;rtptime=%u\r\n", this->track_url_.c_str(),
                             static_cast<unsigned>(this->rtp_seq_), static_cast<unsigned>(this->rtp_ts_));
    }
    this->send_rtsp_response_(str_sprintf("RTSP/1.0 200 OK\r\n%sSession: %u\r\n%s\r\n", cseq_hdr.c_str(),
                                          this->session_id_, rtp_info.c_str()));
    return true;
  }

  if (method == "pause") {
    this->stop_streaming_();
    this->send_rtsp_response_(
        str_sprintf("RTSP/1.0 200 OK\r\n%sSession: %u\r\n\r\n", cseq_hdr.c_str(), this->session_id_));
    return true;
  }

  if (method == "teardown") {
    this->send_rtsp_response_(str_sprintf("RTSP/1.0 200 OK\r\n%s\r\n", cseq_hdr.c_str()));
    this->close_session_();
    return true;
  }

  this->send_rtsp_response_(str_sprintf("RTSP/1.0 501 Not Implemented\r\n%s\r\n", cseq_hdr.c_str()));
  return true;
}

void RtspAudioComponent::start_streaming_() {
  if (!this->allocate_stream_buffers_()) {
    ESP_LOGE(TAG, "Cannot start streaming: buffers unavailable");
    return;
  }
  this->ring_buffer_->reset();

  // Initial RTP state per RFC 3550: random SSRC and starting seq/timestamp.
  this->rtp_ssrc_ = esp_random() | 1U;
  this->rtp_seq_ = static_cast<uint16_t>(esp_random());
  if (this->rtp_seq_ == 0)
    this->rtp_seq_ = 1;
  this->rtp_ts_ = esp_random();

  this->streaming_ = true;
  this->last_rtp_usec_ = esp_timer_get_time();

  // Reset streaming diagnostics for this session.
  this->rtp_packets_sent_ = 0;
  this->bytes_sent_ = 0;
  this->stats_last_packets_ = 0;
  this->last_stats_usec_ = this->last_rtp_usec_;
  this->last_packet_usec_ = this->last_rtp_usec_;
  this->first_packet_logged_ = false;
  this->underrun_warned_ = false;
  this->mic_callbacks_ = 0;
  this->mic_empty_callbacks_ = 0;
  this->mic_bytes_received_ = 0;
  // Reset CPU-use bookkeeping so the first window starts at PLAY, not at
  // boot — otherwise the first published value would average in any idle
  // time the device sat with no client connected.
  this->busy_usec_.store(0, std::memory_order_relaxed);
  this->cpu_window_start_usec_ = this->last_rtp_usec_;
  this->stats_tick_ = 0;
  this->dc_blocker_state_ = {};
  this->lowcut_state_ = {};
  this->highcut_state_ = {};
  this->soft_limiter_state_ = {};
#ifdef USE_SENSOR
  // Drop the carry-over from the previous session so the first 5 s window
  // of this stream isn't biased by stale data.
  this->window_peak_abs_ = 0;
  this->window_min_sl_gain_ = 1.0f;
#endif

  this->mic_source_->start();
  ESP_LOGI(TAG, "Streaming RTP: %u samples/packet (%u ms)", this->samples_per_packet_, this->packet_duration_ms_);
}

void RtspAudioComponent::stop_streaming_() {
  if (!this->streaming_)
    return;
  this->streaming_ = false;
  if (this->mic_source_ != nullptr)
    this->mic_source_->stop();
#ifdef USE_SENSOR
  // Drop the peak meter to its silence floor whenever the mic stops
  // delivering samples — covers both RTSP `PAUSE` (handled here only,
  // close_session_ is not called) and `TEARDOWN` (via close_session_
  // → stop_streaming_). Without this the meter latches at the last
  // live reading until the next 5 s stats tick happens to fire on the
  // already-reset window_peak_abs_. Also zero the running window so
  // any tick that fires while paused doesn't republish stale audio.
  this->window_peak_abs_ = 0;
  if (this->peak_level_dbfs_sensor_ != nullptr && this->peak_level_published_dbfs_ != SILENCE_FLOOR_DBFS) {
    this->peak_level_published_dbfs_ = SILENCE_FLOOR_DBFS;
    this->peak_level_dbfs_sensor_->publish_state(static_cast<float>(SILENCE_FLOOR_DBFS));
  }
  this->window_min_sl_gain_ = 1.0f;
  if (this->limiter_gain_reduction_db_sensor_ != nullptr && this->limiter_gr_published_db_ != 0) {
    this->limiter_gr_published_db_ = 0;
    this->limiter_gain_reduction_db_sensor_->publish_state(0.0f);
  }
#endif
}

void RtspAudioComponent::close_session_() {
  this->stop_streaming_();
  this->rtp_socket_.reset();
  std::memset(&this->client_rtp_addr_, 0, sizeof(this->client_rtp_addr_));
  if (this->control_socket_) {
    this->control_socket_->shutdown(SHUT_RDWR);
    this->control_socket_.reset();
  }
  this->rx_buffer_.clear();
  this->tx_buffer_.clear();
  this->session_active_ = false;
  this->interleaved_ = false;
  // Defer ring-buffer / RTP-packet free until the mic task has actually
  // exited. mic_source_->stop() above is async; a trailing callback may
  // still be inside the data-callback lambda. loop() does the dealloc
  // once mic_source_->is_stopped() flips true.
  this->teardown_guard_.arm();
  this->publish_session_state_();
#ifdef USE_SENSOR
  if (this->bytes_sent_sensor_ != nullptr && this->bytes_sent_published_ != 0) {
    this->bytes_sent_published_ = 0;
    this->bytes_sent_sensor_->publish_state(0);
  }
  // Force the CPU-use sensor back to 0 on session end so the HA gauge doesn't
  // latch at the last live value while the device sits idle waiting for the
  // next client. Compare in tenths so we don't republish identical zeros.
  if (this->cpu_use_pct_sensor_ != nullptr && this->cpu_use_published_tenths_ != 0) {
    this->cpu_use_published_tenths_ = 0;
    this->cpu_use_pct_sensor_->publish_state(0.0f);
  }
  // Note: the peak meter's silence-floor publish lives in stop_streaming_
  // (called above) so PAUSE gets the same treatment as TEARDOWN.
#endif
  ESP_LOGI(TAG, "RTSP session closed");
}

void RtspAudioComponent::publish_session_state_() {
#ifdef USE_BINARY_SENSOR
  if (this->client_connected_bs_ != nullptr)
    this->client_connected_bs_->publish_state(this->session_active_);
#endif
#ifdef USE_TEXT_SENSOR
  if (this->client_ip_ts_ != nullptr) {
    std::string ip;
    if (this->session_active_ && this->control_socket_) {
      sockaddr_storage peer{};
      socklen_t peer_len = sizeof(peer);
      if (this->control_socket_->getpeername(reinterpret_cast<sockaddr *>(&peer), &peer_len) == 0 &&
          peer.ss_family == AF_INET) {
        // IPv4 only: the rest of the component is also IPv4-only (UDP SETUP
        // rejects IPv6, see handle_rtsp_message_), and ESP-IDF's default lwip
        // build leaves sockaddr_in6 incomplete so a v6 branch wouldn't link.
        // An IPv6-over-TCP-interleaved client gets reported as empty.
        char buf[INET_ADDRSTRLEN] = {0};
        if (inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in *>(&peer)->sin_addr, buf, sizeof(buf)) != nullptr)
          ip = buf;
      }
    }
    if (ip != this->client_ip_published_) {
      this->client_ip_published_ = ip;
      this->client_ip_ts_->publish_state(ip);
    }
  }
#endif
}

void RtspAudioComponent::check_session_inactivity_() {
  if (!internal::session_is_idle(esp_timer_get_time(), this->last_rtsp_activity_usec_, SESSION_TIMEOUT_SECONDS))
    return;
  ESP_LOGW(TAG, "RTSP session idle > %us; closing", SESSION_TIMEOUT_SECONDS);
  this->close_session_();
}

void RtspAudioComponent::maybe_send_rtp_() {
  if (!this->streaming_ || this->ring_buffer_ == nullptr || this->rtp_packet_ == nullptr)
    return;
  // UDP needs the RTP socket; interleaved mode rides the control socket.
  if (this->interleaved_ ? (this->control_socket_ == nullptr) : (this->rtp_socket_ == nullptr))
    return;

  const int64_t now = esp_timer_get_time();

  // Catch-up pacing. Sending at most one packet per loop() and resetting the
  // deadline to `now` structurally under-drains the ring buffer: the effective
  // send interval becomes ceil(packet_ms / loop_ms) * loop_ms, which is always
  // >= packet_ms. The mic keeps producing at the real rate, so the buffer fills
  // and eventually overflows. Instead, send as many packets as the elapsed time
  // allows and advance the deadline by exactly one interval each time, so the
  // average send rate matches the audio production rate regardless of loop().
  constexpr int MAX_PACKETS_PER_LOOP = 8;
  int sent = 0;
  while (sent < MAX_PACKETS_PER_LOOP && now - this->last_rtp_usec_ >= static_cast<int64_t>(this->rtp_interval_usec_)) {
    if (!this->send_one_rtp_packet_())
      break;  // no buffered audio yet / socket busy: retry next loop, don't advance deadline
    this->last_rtp_usec_ += this->rtp_interval_usec_;
    sent++;
  }

  // Hit the per-loop cap and still behind (a long stall, e.g. a flash write).
  // Snap the deadline to now so we don't burst MAX_PACKETS_PER_LOOP every loop
  // indefinitely; the backlog is dropped in favour of staying near real time.
  if (sent == MAX_PACKETS_PER_LOOP && now - this->last_rtp_usec_ > static_cast<int64_t>(this->rtp_interval_usec_)) {
    ESP_LOGW(TAG, "RTP pacing behind by >%d packets; resyncing", MAX_PACKETS_PER_LOOP);
    this->last_rtp_usec_ = now;
  }

  this->log_stream_stats_(now);
}

void RtspAudioComponent::log_stream_stats_(int64_t now) {
  // One-time confirmation that media is actually leaving the device, including
  // the destination so it can be checked against the RTSP client's address.
  if (this->rtp_packets_sent_ > 0 && !this->first_packet_logged_) {
    this->first_packet_logged_ = true;
    if (this->interleaved_) {
      ESP_LOGI(TAG, "First RTP packet sent (TCP-interleaved, channel %u)", static_cast<unsigned>(this->rtp_channel_));
    } else {
      auto *addr4 = reinterpret_cast<sockaddr_in *>(&this->client_rtp_addr_);
      const uint32_t ip = ntohl(addr4->sin_addr.s_addr);
      ESP_LOGI(TAG, "First RTP packet sent to %u.%u.%u.%u:%u", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF,
               ip & 0xFF, ntohs(addr4->sin_port));
    }
  }

  // Underrun: streaming but nothing sent for over a second. Almost always the
  // microphone not delivering audio into the ring buffer.
  if (now - this->last_packet_usec_ > 1'000'000) {
    if (!this->underrun_warned_) {
      this->underrun_warned_ = true;
      ESP_LOGW(TAG,
               "No RTP sent for >1s. Mic totals: %u callbacks (%u empty), %u bytes; ring buffer %zu bytes. "
               "If these totals are still rising, this is a transient send-side stall (e.g. Wi-Fi "
               "activity) and is harmless once the stream recovers. If the totals are frozen, the "
               "microphone stopped: 0 callbacks = MicrophoneSource not delivering; callbacks but "
               "0 bytes = I2S read no audio.",
               this->mic_callbacks_, this->mic_empty_callbacks_, this->mic_bytes_received_,
               this->ring_buffer_->available());
    }
  } else {
    this->underrun_warned_ = false;
  }

  // Periodic throughput so a healthy stream is visible in the log and a stalled
  // one stands out. ~250 packets per 5 s window is expected at the default
  // 20 ms packet cadence (50 packets/s).
  if (now - this->last_stats_usec_ >= 5'000'000) {
    const uint32_t pkts = this->rtp_packets_sent_ - this->stats_last_packets_;
    ESP_LOGD(TAG, "RTP stream: %u packets/5s; mic: %u callbacks, %u empty, %u bytes; ring buffer %zu bytes", pkts,
             this->mic_callbacks_, this->mic_empty_callbacks_, this->mic_bytes_received_,
             this->ring_buffer_->available());
    this->last_stats_usec_ = now;
    this->stats_last_packets_ = this->rtp_packets_sent_;
#ifdef USE_SENSOR
    // Piggyback on the 5 s window so HA sees one update per period rather than
    // one per packet. Only publish on change to avoid filling the HA recorder
    // when the stream is paused at a stable byte count.
    if (this->bytes_sent_sensor_ != nullptr && this->bytes_sent_ != this->bytes_sent_published_) {
      this->bytes_sent_published_ = this->bytes_sent_;
      this->bytes_sent_sensor_->publish_state(this->bytes_sent_);
    }
    // Peak meter (post-gain). 5 s window matches bytes_sent and is part of
    // standardising the diagnostic publish rate. Floor at SILENCE_FLOOR_DBFS
    // so silent windows are a real, in-band number rather than -inf, which
    // keeps HA's history graph continuous and avoids surprising downstream
    // filters/formulas.
    if (this->peak_level_dbfs_sensor_ != nullptr) {
      int16_t dbfs;
      if (this->window_peak_abs_ == 0) {
        dbfs = SILENCE_FLOOR_DBFS;
      } else {
        // INT16_MAX (32767) is full scale; 32768 is the conventional
        // 0 dBFS reference so that INT16_MIN's magnitude doesn't read as
        // a fraction of a dB over 0.
        const float ratio = static_cast<float>(this->window_peak_abs_) / 32768.0f;
        const float raw = 20.0f * std::log10(ratio);
        const int rounded = static_cast<int>(std::lround(raw));
        if (rounded < SILENCE_FLOOR_DBFS)
          dbfs = SILENCE_FLOOR_DBFS;
        else if (rounded > 0)
          dbfs = 0;
        else
          dbfs = static_cast<int16_t>(rounded);
      }
      if (dbfs != this->peak_level_published_dbfs_) {
        this->peak_level_published_dbfs_ = dbfs;
        this->peak_level_dbfs_sensor_->publish_state(static_cast<float>(dbfs));
      }
      this->window_peak_abs_ = 0;
    }
    // Limiter gain-reduction meter. Max reduction in the 5 s window, expressed
    // as positive dB (0 = no limiting, 3 = 3 dB applied). When the limiter is
    // bypassed, always publishes 0 (no reduction). Mirrors the peak-level
    // publish-on-change pattern; resets the window after each publish.
    if (this->limiter_gain_reduction_db_sensor_ != nullptr) {
      int16_t gr_db;
      if (this->soft_limiter_bypass_ || this->window_min_sl_gain_ >= 1.0f) {
        gr_db = 0;
      } else {
        const float raw = -20.0f * std::log10(this->window_min_sl_gain_);
        const int rounded = static_cast<int>(std::lround(raw));
        // Clamp: gain is in (0,1] so reduction is in [0, ∞); practical ceiling ~40 dB.
        gr_db = static_cast<int16_t>(rounded < 0 ? 0 : (rounded > 40 ? 40 : rounded));
      }
      if (gr_db != this->limiter_gr_published_db_) {
        this->limiter_gr_published_db_ = gr_db;
        this->limiter_gain_reduction_db_sensor_->publish_state(static_cast<float>(gr_db));
      }
      this->window_min_sl_gain_ = 1.0f;
    }
    // CPU-use: publish every other 5 s stats tick (~10 s cadence) so we get a
    // smooth, low-churn signal. The percentage is (busy µs spent in our loop +
    // mic callback) / (elapsed wall-clock µs since the last publish). Excludes
    // Wi-Fi/LwIP, the I²S driver, and other ESPHome components; see CHANGELOG
    // for the threshold bands users should read this against.
    this->stats_tick_++;
    if (this->cpu_use_pct_sensor_ != nullptr && (this->stats_tick_ & 1) == 0) {
      const int64_t busy = this->busy_usec_.exchange(0, std::memory_order_relaxed);
      const int64_t window = now - this->cpu_window_start_usec_;
      this->cpu_window_start_usec_ = now;
      if (window > 0) {
        float pct = static_cast<float>(busy) * 100.0f / static_cast<float>(window);
        if (pct < 0.0f)
          pct = 0.0f;
        if (pct > 100.0f)
          pct = 100.0f;
        // Quantise to tenths-of-percent so publish-on-change is cheap and
        // matches the sensor's declared accuracy_decimals=1.
        const uint16_t tenths = static_cast<uint16_t>(pct * 10.0f + 0.5f);
        if (tenths != this->cpu_use_published_tenths_) {
          this->cpu_use_published_tenths_ = tenths;
          this->cpu_use_pct_sensor_->publish_state(static_cast<float>(tenths) / 10.0f);
        }
      }
    }
#endif
  }
}

bool RtspAudioComponent::send_one_rtp_packet_() {
  const size_t payload_bytes = this->stream_info_.samples_to_bytes(this->samples_per_packet_);

  // Only consume audio once a whole packet is buffered. RingBuffer::read() is
  // destructive even on a short read: calling it with fewer than payload_bytes
  // available consumes and discards a partial packet, so the buffer can never
  // accumulate a full one. Without this guard the send path drains the buffer
  // every loop and RTP stalls at 0 packets even though the mic delivers audio.
  if (this->ring_buffer_->available() < payload_bytes)
    return false;

  uint8_t *header = this->rtp_packet_;
  uint8_t *payload = header + RTP_HEADER_BYTES;

  // Pull host-endian (little-endian on ESP32) PCM straight into the payload area.
  if (this->ring_buffer_->read(payload, payload_bytes, 0) < payload_bytes)
    return false;

  // Write RTP header in network byte order using the standard ESPHome helper.
  header[0] = 0x80;  // V=2, P=0, X=0, CC=0
  header[1] = RTP_PAYLOAD_TYPE & 0x7F;
  const uint16_t seq_be = convert_big_endian(this->rtp_seq_);
  const uint32_t ts_be = convert_big_endian(this->rtp_ts_);
  const uint32_t ssrc_be = convert_big_endian(this->rtp_ssrc_);
  std::memcpy(header + 2, &seq_be, sizeof(seq_be));
  std::memcpy(header + 4, &ts_be, sizeof(ts_be));
  std::memcpy(header + 8, &ssrc_be, sizeof(ssrc_be));

  // Run the per-sample DSP chain (low-cut → high-cut → gain → L16
  // byteswap) in one pass over the payload. The pipeline header
  // centralises the stage order so future stages (soft limiter, gain
  // smoothing) don't touch this file.
  const uint16_t packet_peak_abs = internal::process_l16_payload_inplace(
      reinterpret_cast<int16_t *>(payload), this->samples_per_packet_, this->dc_blocker_state_, this->lowcut_state_,
      this->lowcut_coeffs_, this->lowcut_bypass_, this->highcut_state_, this->highcut_coeffs_, this->highcut_bypass_,
      this->gain_q8_.load(std::memory_order_relaxed), this->soft_limiter_state_, this->soft_limiter_bypass_,
      this->soft_limiter_threshold_linear_, this->soft_limiter_attack_coeff_, this->soft_limiter_release_coeff_);
#ifdef USE_SENSOR
  // One compare per packet (~50 Hz) regardless of sample rate — the
  // per-sample work is already inside the pipeline. Windows reset in
  // log_stream_stats_ after each 5 s publish.
  if (packet_peak_abs > this->window_peak_abs_)
    this->window_peak_abs_ = packet_peak_abs;
  if (!this->soft_limiter_bypass_) {
    const float mg = this->soft_limiter_state_.packet_min_gain;
    if (mg < this->window_min_sl_gain_)
      this->window_min_sl_gain_ = mg;
  }
#else
  (void)packet_peak_abs;
#endif

  const size_t packet_len = RTP_HEADER_BYTES + payload_bytes;

  if (this->interleaved_) {
    // Frame the packet on the RTSP TCP connection: '$' + channel + 16-bit
    // length + RTP. If the client is not draining the socket, drop whole
    // packets (never a partial one — that would corrupt the framing) but still
    // advance seq/ts so the receiver sees an ordinary loss rather than a stall.
    if (this->tx_buffer_.size() + INTERLEAVE_HEADER_BYTES + packet_len <= MAX_RTP_BACKLOG_BYTES) {
      const uint8_t framing[INTERLEAVE_HEADER_BYTES] = {'$', this->rtp_channel_,
                                                        static_cast<uint8_t>((packet_len >> 8) & 0xFF),
                                                        static_cast<uint8_t>(packet_len & 0xFF)};
      this->tx_buffer_.append(reinterpret_cast<const char *>(framing), INTERLEAVE_HEADER_BYTES);
      this->tx_buffer_.append(reinterpret_cast<const char *>(header), packet_len);
      this->rtp_packets_sent_++;
      this->bytes_sent_ += packet_len;
      this->last_packet_usec_ = esp_timer_get_time();
    }
    this->rtp_seq_++;
    this->rtp_ts_ += this->samples_per_packet_;
    return true;
  }

  // UDP transport.
  ssize_t sent = this->rtp_socket_->sendto(header, packet_len, 0, reinterpret_cast<sockaddr *>(&this->client_rtp_addr_),
                                           sizeof(sockaddr_in));
  if (sent < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return false;
    ESP_LOGW(TAG, "RTP sendto errno=%d", errno);
    return false;
  }
  if (static_cast<size_t>(sent) != packet_len) {
    ESP_LOGW(TAG, "RTP short send %zd/%zu", sent, packet_len);
    return false;
  }

  this->rtp_seq_++;
  this->rtp_ts_ += this->samples_per_packet_;
  this->rtp_packets_sent_++;
  this->bytes_sent_ += packet_len;
  this->last_packet_usec_ = esp_timer_get_time();
  return true;
}

}  // namespace esphome::rtsp_audio

#endif  // USE_RTSP_AUDIO
