# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog 1.1.0](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning 2.0.0](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **CPU-use diagnostic sensor (`cpu_use_pct`).** Opt-in `sensor:` entry on
  `platform: rtsp_audio` that reports what fraction of wall-clock the RTSP
  audio path is consuming, so the user can answer the practical question
  *"is this ESP chip up to the task, or do I need a more powerful one /
  do I need to disable a feature?"* without guessing. The percentage is
  self-measured: an RAII scope guard times the component's `loop()` body
  and the mic data callback with `esp_timer_get_time()`, accumulates the
  µs into an atomic counter, and every ~10 s (piggybacked on the existing
  5 s stats window) divides that by elapsed wall-clock to publish a value
  between 0 and 100 % with one decimal. **Interpretation:** under ~30 %
  is plenty of headroom; 30–70 % is normal; 70–90 % means running hot —
  brief audio glitches under Wi-Fi load are possible and you should
  consider simplifying the DSP (raise the low-cut to skip the IIR work,
  drop the high-cut filter, lower gain to take the bit-identical path);
  sustained >90 % means the chip is at its limit; 100 % means upgrade to
  a more capable board (e.g. ESP32-S2 → ESP32-S3) or disable features.
  The metric excludes Wi-Fi/LwIP, the I²S driver, and other ESPHome
  components, so treat it as a lower bound on total system load. The
  measurement itself costs well under 0.1 % of CPU (two `esp_timer`
  reads and a relaxed atomic add per loop tick / mic callback), so it
  does not meaningfully bias the value it reports. Resets at each `PLAY`
  and publishes 0 on session close so the HA gauge doesn't latch at the
  last live value when no client is connected. See
  [docs/configuration.md](docs/configuration.md#diagnostic-sensors).
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
  platform exposes `low_cut_frequency_hz` to HA as a slider with
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
