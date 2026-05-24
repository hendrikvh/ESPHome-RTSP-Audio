# ESPHome RTSP Audio

[![CI](https://github.com/hendrikvh/ESPHome-RTSP-Audio/actions/workflows/ci.yml/badge.svg)](https://github.com/hendrikvh/ESPHome-RTSP-Audio/actions/workflows/ci.yml)
[![Latest release](https://img.shields.io/github/v/release/hendrikvh/ESPHome-RTSP-Audio?include_prereleases&sort=semver)](https://github.com/hendrikvh/ESPHome-RTSP-Audio/releases)

See [CHANGELOG.md](CHANGELOG.md) for release notes.

**Status:** Early development. Buggy and unreliable.

External component to stream RTSP audio using ESPHome.

## Features

- **RTP over UDP** (`RTP/AVP`) — the classic transport. works with VLC and most media players.
- **RTP over TCP** (`RTP/AVP/TCP`, interleaved) — RTP framed on the RTSP connection. Works with FFmpeg, BirdNET-Go, Frigate, and most NVRs.
- Transport is negotiated per client at `SETUP`, so UDP and TCP clients both work with no configuration.
- Uncompressed **L16 PCM** audio — 16 kHz mono 16-bit, RTP payload type 96 (`L16/16000/1`).
- One client at a time.
- **Diagnostic sensors for Home Assistant** — opt-in binary_sensor / text_sensor / sensor platforms expose client-connected state, client IP, and bytes sent so you can debug the stream from HA without needing to tail logs. See [docs/configuration.md#diagnostic-sensors](docs/configuration.md#diagnostic-sensors).
- **CPU-use sensor** — opt-in `cpu_use_pct` sensor reports the percentage of wall-clock that the RTSP audio path (main `loop()` + mic data callback) is consuming, so you can see whether your ESP chip has headroom for the current sample rate, DSP settings, and enabled features — and decide whether you need a more powerful chip or have to disable a feature. **Interpretation:** under ~30 % is plenty of headroom; 30–70 % is normal; 70–90 % means brief audio glitches under Wi-Fi load are possible and you should consider simplifying the DSP; sustained >90 % (and especially 100 %) means the chip is at its limit — upgrade to a more capable board (e.g. ESP32-S2 → ESP32-S3) or back off DSP / sample rate. Excludes Wi-Fi/LwIP, the I²S driver, and other ESPHome components, so treat it as a lower bound on total system load. See [docs/configuration.md#diagnostic-sensors](docs/configuration.md#diagnostic-sensors).
- **High and Low Cut filters** — complementary low-cut and high-cut stages tunable from Home Assistant, both negligible CPU cost (one-pole IIR, one MAC per sample). See [docs/configuration.md#cut-filters](docs/configuration.md#cut-filters).
  - **Low-cut** — strips MEMS-mic DC bias and low-frequency rumble (HVAC, handling, wind) so downstream consumers get a cleaner signal with more usable dynamic range. Always on; defaults to **100 Hz** (leaves voice fundamentals intact). Raise it in noisier rooms or for non-voice sources.
  - **High-cut** — rolls off out-of-band hiss above the cutoff. **Off by default** (cutoff = 20 kHz, bit-identical passthrough). Useful for narrow-band voice models or NVR storage where the top end is wasted bandwidth.
- **Input gain in dB, tunable from Home Assistant** — software level adjustment applied after the cut filters. Default is **0 dB** (unity, bit-identical to a no-gain build); the slider spans **−20 dB to +40 dB** (1 dB steps), persisted across reboots. Overflow is saturating-clamped, never wrapped. See [docs/configuration.md#input-gain](docs/configuration.md#input-gain).

## Under the hood

- **PSRAM-aware buffers.** Ring buffer and RTP packet buffer prefer external RAM and fall back to internal heap automatically on boards without PSRAM.
- **Lazy session memory.** Audio buffers are allocated on `PLAY` and freed on `TEARDOWN`, Wi-Fi loss, or peer close, so idle nodes carry no audio buffer overhead.
- **Self-recovery on Wi-Fi drop.** The active session is torn down cleanly and the RTSP listener stays up ready for the next client.
- **Session inactivity timeout to handle unexpected client disconnects.** A client that vanishes without sending `TEARDOWN` ( due to crash, sleep, peer Wi-Fi drop) is reaped after 60 s so the single client slot frees up, ready for the next connection
- **OOM-safe RTP send path.** The transmit buffer is reserved up front so no-PSRAM boards do not run out of memory mid-stream.

## Hardware

A **dual-core ESP32 is recommended, e.g. the ESP32-S3.** On single-core chips
(such as the ESP32-S2 that I'm testing on) the Wi-Fi stack and the audio loop
share one CPU core, so occasional brief (~1 s) audio gaps occur when Wi-Fi gets
busy. A dual-core chip runs Wi-Fi and the audio loop on separate cores and avoids this.

Enable the `cpu_use_pct` diagnostic sensor (see
[docs/configuration.md#diagnostic-sensors](docs/configuration.md#diagnostic-sensors))
to confirm whether your chosen board has enough headroom for your configuration
before committing to it — that's the recommended way to decide between staying
on what you have, upgrading to an ESP32-S3, or disabling features.

### Tested with

* [ESP32-S2](https://www.wemos.cc/en/latest/s2/s2_mini.html)
with [INMP441 MEMS microphone module](https://easyelecmodule.com/a-complete-guide-to-the-inmp441-i2s-microphone/).

## Goals

- Stream microphone audio off an ESP32 over standard RTSP
- Simple: Single stream, fixed 16 kHz / mono / 16-bit audio
- Pair cleanly with ESPHome's mic stack

## Design philosophy

- Use ESPHome built-ins over custom code whenever possible
- ESP-IDF first platform first sine this is the future of ESPHome

## Intentional non-goals

### Only a single stream/client is supported at a time

Only one RTSP client is served at a time and a second TCP connection is
rejected. A single ring buffer drains into a single transport. This
avoids the need for per-client packet pacing, SSRC, and sequence numbering.

### Sample-rate conversion

The microphone source is set to 16 kHz mono 16-bit so PCM passes
straight into RTP with no resampler pulled in.

## Development

All tests and firmware builds run inside Docker — only `docker` is
required on the host. Use the [`Makefile`](Makefile):

```
make help            # list targets
make config          # esphome config on each test YAML
make compile         # full firmware build for each test YAML
make compile BOARD=s3-idf
make test            # host C++ unit tests (gtest, ctest) in Docker
make lint            # pre-commit hooks (clang-format, ruff, ...)
```

See [`CONTRIBUTING.md`](CONTRIBUTING.md) for details and the CI matrix
policy.
