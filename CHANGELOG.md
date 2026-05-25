# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog 1.1.0](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning 2.0.0](https://semver.org/spec/v2.0.0.html).

## [0.1.0] - 2026-05-25

### Changed

- **Audio sample rate raised from 16 kHz to 32 kHz.** The mic source, RTP
  payload (`L16/32000/1`), filter coefficients, and high-cut bounds
  (now capped at the new 16 kHz Nyquist) all move together. Bit depth
  stays at 16-bit mono. The MEMS capsules this targets (e.g. INMP441)
  can't faithfully capture much above 16 kHz, so 32 kHz is the balance
  point — voice plus the ambient detail (footsteps, doors, bird calls)
  in the 8–16 kHz octave, without spending CPU and Wi-Fi bandwidth on
  content the mic can't reproduce. On an ESP32-S3 the audio path still
  sits well under the comfort budget reported by `cpu_use_pct`. RTP
  packets roughly double in size (~1.3 KB at 20 ms), still well under
  MTU. See the README's "Audio format" section.
- **Ring buffer reduced from 2 s to 1 s of jitter slack** so the
  component fits on bare ESP32 boards without PSRAM. At 32 kHz the
  2 s buffer was 128 KB, which routinely fails to allocate as a
  contiguous chunk on `WROOM` / RISC-V parts that only have ~320 KB
  internal SRAM total. 1 s (~64 KB) keeps the same byte budget as the
  previous 16 kHz / 2 s buffer; the RTSP client's own jitter buffer
  brings end-to-end resilience back to ~1.5–2 s. See the README's
  "Sized to fit ESP32 internal SRAM" section for the rationale and a
  note on possible PSRAM-aware sizing in future.

### Added

- **Peak-level diagnostic sensor (`peak_level_dbfs`).** Opt-in `sensor:`
  on `platform: rtsp_audio` reporting peak audio level in dBFS
  (0 dBFS = clipping) over the last 5 s window. Designed as the
  missing feedback channel for the `gain_db` slider — previously the
  user had to guess the right gain by listening to the receiving
  client. See
  [docs/configuration.md](docs/configuration.md#reading-peak_level_dbfs).
- **CPU-use diagnostic sensor (`cpu_use_pct`).** Opt-in `sensor:` on
  `platform: rtsp_audio` reporting what fraction of wall-clock the
  RTSP audio path consumes, so you can tell whether the chip has
  headroom for the current sample rate, DSP settings, and enabled
  features — and decide whether to upgrade or back off. See
  [docs/configuration.md](docs/configuration.md#diagnostic-sensors).
- **Audio gain in dB, tunable from Home Assistant.** Opt-in `number:`
  on `platform: rtsp_audio` exposing a software audio gain slider
  (`gain_db`, -20 to +40 dB, 1 dB step, default 0 dB, persisted) so a
  quiet mic can be lifted without re-flashing. 0 dB is bit-identical
  to a no-gain build; overflow is saturating-clamped, never wrapped.
  See [docs/configuration.md](docs/configuration.md#audio-gain).
- **High-cut filter, tunable from Home Assistant.** Opt-in `number:` on
  `platform: rtsp_audio` exposing a high-cut frequency slider
  (`high_cut_frequency_hz`, default 20000 Hz / off, 1000–20000 Hz
  range) that rolls off energy above the cutoff — useful for taming
  mic hiss, wind noise, and out-of-band content downstream consumers
  don't need (narrow-band voice models, NVR storage). At the maximum
  the stage is skipped and the audio path is bit-identical to a build
  without it. See
  [docs/configuration.md](docs/configuration.md#low-and-high-cut-filters).
- **Low-cut filter, tunable from Home Assistant.** Opt-in `number:` on
  `platform: rtsp_audio` exposing a low-cut frequency slider
  (`low_cut_frequency_hz`, default 100 Hz, 20–500 Hz range) that
  strips MEMS-mic DC bias and sub-100 Hz rumble (HVAC, handling, wind)
  before the audio leaves the device, so downstream consumers get a
  cleaner signal with more usable dynamic range. See
  [docs/configuration.md](docs/configuration.md#low-and-high-cut-filters).
- **Diagnostic sensors for Home Assistant.** Opt-in `client_connected`
  (binary_sensor), `client_ip` (text_sensor), and `bytes_sent` (sensor)
  so an HA dashboard can see in real time whether a client is pulling
  audio, who it is, and at what bitrate without watching serial logs.
  See
  [docs/configuration.md](docs/configuration.md#diagnostic-sensors).
- **RTSP session inactivity timeout to better handle silent client disconnects.**
  A client that disappears without sending `TEARDOWN` (ue to crash,
  sleep, Wi-Fi drop) is now reaped after 60 s instead of blocking the
  single client slot for minutes until the OS-level TCP timeout fires.

### Fixed

- **Use-after-free on session teardown
  ([#5](https://github.com/hendrikvh/ESPHome-RTSP-Audio/issues/5)).**
  `deallocate_stream_buffers_()` could free the ring buffer while a
  trailing I²S DMA callback on the mic's own FreeRTOS task was still
  writing into it, occasionally crashing the node on `TEARDOWN` /
  Wi-Fi drop / peer close. Deallocation is now deferred until
  `mic_source_->is_stopped()` confirms the mic task has actually
  exited, so the writer is guaranteed gone before the buffer goes
  away.

## [0.0.1] - 2026-05-23

_First semi-stable release. Working nicely with BirdNET-Go so my wife is happy!_

### Added

- Single-viewer RTSP/1.0 server on ESP32 + ESP-IDF, exposing one
  microphone as a stream.
- **TCP and UDP RTP transports**, both supported. Transport is
  negotiated per client at `SETUP`: UDP (`RTP/AVP`) works with VLC
  and most players; TCP-interleaved (`RTP/AVP/TCP`) works with
  FFmpeg, BirdNET-Go, Frigate, and most NVRs. No YAML toggle.
- Tests covering ESP32-S2 + ESP-IDF and ESP32-S3 + ESP-IDF,
  run on every PR (S3 only) and on push to `main` (both) via GitHub
  Actions.
- Local Dockerised development workflow: `make config`,
  `make compile`, `make lint`. Only Docker required on the host.

### Fixed

- Crash on no-PSRAM boards. `tx_buffer_` is now reserved up front so
  the RTP send path no longer triggers OOM at runtime.

[Unreleased]: https://github.com/hendrikvh/ESPHome-RTSP-Audio/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/hendrikvh/ESPHome-RTSP-Audio/compare/v0.0.1...v0.1.0
[0.0.1]: https://github.com/hendrikvh/ESPHome-RTSP-Audio/releases/tag/v0.0.1
