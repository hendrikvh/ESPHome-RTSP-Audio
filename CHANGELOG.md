# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog 1.1.0](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning 2.0.0](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **Input gain in dB, tunable from Home Assistant.** Software gain
  stage applied after the low-cut filter and before the L16 byteswap,
  so the user can lift a quiet mic (or back off a loud one) without
  re-flashing or touching the I²S `gain_factor`. Exposed as an opt-in
  `number: platform: rtsp_audio` entity (`gain_db`) with a
  **−20 dB to +40 dB** range (1 dB step, max linear ≈ 100×),
  default **0 dB** (unity), persisted across reboots. At exactly 0 dB
  the gain stage is skipped and the RTP byte stream is bit-identical
  to a build without the feature. Overflow is **saturating-clamped**
  (never wraps). Per-sample math is integer-only Q8 linear; the dB
  conversion happens once per slider move, never per sample. See
  [docs/configuration.md](docs/configuration.md#input-gain).
- **Low-cut filter (DC blocker / high-pass), tunable from Home Assistant.**
  Configurable one-pole IIR low-cut filter applied to every sample
  before it leaves the device. MEMS microphones like the INMP441
  ship with a small DC bias and pick up a lot of sub-100 Hz energy
  (HVAC rumble, handling, wind). That offset wastes dynamic range,
  and the low-frequency content makes any later gain stage clip
  earlier and thump audibly on level changes — removing both at the
  source gives downstream consumers (Frigate, BirdNET-Go, voice
  pipelines, NVRs) a cleaner signal to work with. Default frequency
  is **100 Hz** (preserves voice fundamentals); an opt-in `number:`
  platform exposes `lowcut_filter_frequency` to HA as a slider with
  values from 20 Hz to 500 Hz, persisted across reboots.
  Integer-only Q15 math in the hot path; negligible CPU cost. See
  [docs/configuration.md](docs/configuration.md#audio-processing).
- **Diagnostic sensors for Home Assistant.** Opt-in `client_connected`
  (binary_sensor), `client_ip` (text_sensor), and `bytes_sent` (sensor)
  so an HA dashboard can see in real time whether a client is pulling
  audio, who it is, and at what bitrate without watching serial logs.
  All three are opt-in via separate platform blocks and tagged
  `entity_category: diagnostic`. See
  [docs/configuration.md](docs/configuration.md#diagnostic-sensors).
- **RTSP session inactivity timeout to better handle silent client disconnects.**
  A client that disappears without sending `TEARDOWN` (ue to crash,
  sleep, Wi-Fi drop) is now reaped after 60 s instead of blocking the
  single client slot for minutes until the OS-level TCP timeout fires.

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

[Unreleased]: https://github.com/hendrikvh/ESPHome-RTSP-Audio/compare/v0.0.1...HEAD
[0.0.1]: https://github.com/hendrikvh/ESPHome-RTSP-Audio/releases/tag/v0.0.1
