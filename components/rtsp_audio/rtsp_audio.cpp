#include "rtsp_audio.h"
#ifdef USE_RTSP_AUDIO

#include <esp_random.h>
#include <esp_timer.h>
#include <strings.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "esphome/components/network/util.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome::rtsp_audio {

static const char *const TAG = "rtsp_audio";

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
  // FINAL_VALIDATE_SCHEMA already pins this to 16 kHz mono 16-bit, but a runtime
  // guard keeps the audio math honest if someone bypasses validation.
  if (this->stream_info_.get_sample_rate() != 16000 || this->stream_info_.get_channels() != 1 ||
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
  ESP_LOGCONFIG(TAG, "  Audio: %u Hz / %u ch / %u bit, %u samples/pkt", this->stream_info_.get_sample_rate(),
                this->stream_info_.get_channels(), this->stream_info_.get_bits_per_sample(), this->samples_per_packet_);
}

void RtspAudioComponent::loop() {
  if (!network::is_connected() && this->control_socket_) {
    ESP_LOGW(TAG, "Network down; closing RTSP session");
    this->close_session_();
    return;
  }
  if (this->listen_socket_ && !this->control_socket_)
    this->try_accept_();
  if (this->control_socket_)
    this->drain_control_socket_();
  this->maybe_send_rtp_();
  this->flush_tx_buffer_();
}

void RtspAudioComponent::attach_mic_callback_() {
  this->mic_source_->add_data_callback([this](const std::vector<uint8_t> &data) {
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
  // RingBuffer::create() already uses RAMAllocator<uint8_t> internally, so
  // ~2 s of jitter slack lands in PSRAM on capable boards automatically.
  if (this->ring_buffer_ == nullptr) {
    const size_t bytes = this->stream_info_.ms_to_bytes(2000);
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
          str_sprintf("RTSP/1.0 200 OK\r\n%sSession: %u;timeout=120\r\n"
                      "Transport: RTP/AVP/TCP;unicast;interleaved=%u-%u\r\n\r\n",
                      cseq_hdr.c_str(), this->session_id_, static_cast<unsigned>(tr.rtp_channel),
                      static_cast<unsigned>(tr.rtcp_channel)));
      this->session_active_ = true;
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
        str_sprintf("RTSP/1.0 200 OK\r\n%sSession: %u;timeout=120\r\n"
                    "Transport: RTP/AVP;unicast;client_port=%u-%u;server_port=%u-%u\r\n\r\n",
                    cseq_hdr.c_str(), this->session_id_, tr.client_rtp_port, tr.client_rtcp_port,
                    this->server_rtp_port_, this->server_rtp_port_ + 1));
    this->session_active_ = true;
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
  this->stats_last_packets_ = 0;
  this->last_stats_usec_ = this->last_rtp_usec_;
  this->last_packet_usec_ = this->last_rtp_usec_;
  this->first_packet_logged_ = false;
  this->underrun_warned_ = false;
  this->mic_callbacks_ = 0;
  this->mic_empty_callbacks_ = 0;
  this->mic_bytes_received_ = 0;

  this->mic_source_->start();
  ESP_LOGI(TAG, "Streaming RTP: %u samples/packet (%u ms)", this->samples_per_packet_, this->packet_duration_ms_);
}

void RtspAudioComponent::stop_streaming_() {
  if (!this->streaming_)
    return;
  this->streaming_ = false;
  if (this->mic_source_ != nullptr)
    this->mic_source_->stop();
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
  this->deallocate_stream_buffers_();
  ESP_LOGI(TAG, "RTSP session closed");
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

  // Byteswap each int16 sample in place to get RFC 3551 L16 (network byte order).
  auto *samples = reinterpret_cast<int16_t *>(payload);
  for (size_t i = 0; i < this->samples_per_packet_; i++)
    samples[i] = convert_big_endian(samples[i]);

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
  this->last_packet_usec_ = esp_timer_get_time();
  return true;
}

}  // namespace esphome::rtsp_audio

#endif  // USE_RTSP_AUDIO
