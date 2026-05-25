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
- Uncompressed **L16 PCM** audio — 32 kHz mono 16-bit, RTP payload type 96 (`L16/32000/1`). See [Audio format](#audio-format) for the rationale.
- One client at a time.
- **Diagnostic sensors for Home Assistant** — opt-in binary_sensor / text_sensor / sensor platforms expose client-connected state, client IP, and bytes sent so you can debug the stream from HA without needing to tail logs. See [docs/configuration.md#diagnostic-sensors](docs/configuration.md#diagnostic-sensors).
- **CPU-use sensor** — opt-in `cpu_use_pct` reports what fraction of wall-clock the audio path is consuming, so you can tell whether your chip has headroom for the current config. See [docs/configuration.md#diagnostic-sensors](docs/configuration.md#diagnostic-sensors).
- **Peak-level sensor** — opt-in `peak_level_dbfs` reports the loudest sample sent in the last 5 s window (dBFS, 0 = clipping). Use it as the feedback channel for the `gain_db` slider. See [docs/configuration.md#reading-peak_level_dbfs](docs/configuration.md#reading-peak_level_dbfs).
- **High and Low Cut filters** — complementary low-cut and high-cut stages tunable from Home Assistant, both negligible CPU cost (one-pole IIR, one MAC per sample). See [docs/configuration.md#low-and-high-cut-filters](docs/configuration.md#low-and-high-cut-filters).
  - **Low-cut** — strips MEMS-mic DC bias and low-frequency rumble (HVAC, handling, wind) so downstream consumers get a cleaner signal with more usable dynamic range. Always on; defaults to **100 Hz** (leaves voice fundamentals intact). Raise it in noisier rooms or for non-voice sources.
  - **High-cut** — rolls off out-of-band hiss above the cutoff. **Off by default** (cutoff = 16 kHz, the Nyquist frequency for our 32 kHz audio — bit-identical passthrough). Useful for narrow-band voice models or NVR storage where the top end is wasted bandwidth.
- **Input gain in dB, tunable from Home Assistant** — software level adjustment applied after the cut filters. Default is **0 dB** (unity, bit-identical to a no-gain build); the slider spans **−20 dB to +40 dB** (1 dB steps), persisted across reboots. Overflow is saturating-clamped, never wrapped. See [docs/configuration.md#input-gain](docs/configuration.md#input-gain).

## Audio format

The mic source is set to **L16 PCM, 32 kHz, mono, 16-bit** (RTP payload type 96, `L16/32000/1`). The format is fixed — no resampler is pulled in, so PCM flows straight from the mic into RTP.

- **Usable bandwidth** — Nyquist sits at 16 kHz, and the high-cut filter caps useful content at or below that. Comfortably above the ~8 kHz where speech intelligibility lives, and well into the range a typical MEMS capsule (e.g. INMP441) can reproduce cleanly.
- **Why 32 kHz, not higher or lower** — the MEMS mics this targets are not high-fidelity capsules. Pushing past 32 kHz spends CPU and Wi-Fi bandwidth on content the mic can't faithfully capture; staying at 16 kHz leaves voice fine but throws away the ambient detail (footsteps, doors, bird calls) that lives in the 8–16 kHz octave. 32 kHz is the balance point.
- **CPU cost** — roughly double the 16 kHz path, but on an ESP32-S3 the audio path still sits well under the comfort budget described in the CPU-use sensor section above.
- **Wi-Fi / packet size** — RTP packets are ~1.3 KB at the default 20 ms packet duration, still well under MTU. Both UDP and TCP transports are unaffected.

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
- Simple: Single stream, fixed 32 kHz / mono / 16-bit audio
- Pair cleanly with ESPHome's mic stack

## Design philosophy

- Use ESPHome built-ins over custom code whenever possible
- ESP-IDF first platform first sine this is the future of ESPHome

### Sized to fit ESP32 internal SRAM (no PSRAM required)

The jitter ring buffer is sized for **1 second of audio** (~64 KB at 32 kHz mono 16-bit). That's a deliberate compromise: 2 s would absorb longer Wi-Fi stalls, but it would also push the buffer past what a bare ESP32 can hand out as a single contiguous chunk of internal RAM.

The ESP32 family is split on PSRAM:

- **No PSRAM** — bare `ESP32-WROOM`, most `ESP32-S3-WROOM-1` SKUs without an `R` in the part number, and all current `ESP32-C3` / `-C6` / `-H2` parts (the RISC-V chips don't even support PSRAM). These boards have ~320 KB of internal SRAM total, most of it claimed by Wi-Fi, LwIP, and the ESPHome runtime — a 128 KB contiguous allocation routinely fails on them.
- **With PSRAM** — `WROVER` variants, `ESP32-S3-WROOM-1-N…R8` / `-R2`, and PSRAM-equipped S2 modules (e.g. LOLIN S2 Mini). 2 MB or 8 MB external RAM is plenty.

Because we want the component to "just work" on any ESPHome-capable ESP32 — including the cheap WROOM boards everyone has in a drawer — the buffer is sized for the no-PSRAM case. The RTSP client adds its own jitter buffer (VLC, ffmpeg, NVRs all hold ~500 ms–1 s before playback starts), so end-to-end resilience is closer to 1.5–2 s.

A future enhancement would be to detect PSRAM at runtime and grow the ring buffer to 2 s when it's available, giving PSRAM boards the extra safety margin without breaking the bare-WROOM path.

## Intentional non-goals

### Only a single stream/client is supported at a time

Only one RTSP client is served at a time and a second TCP connection is
rejected. A single ring buffer drains into a single transport. This
avoids the need for per-client packet pacing, SSRC, and sequence numbering.

### Sample-rate conversion

The microphone source is set to 32 kHz mono 16-bit so PCM passes
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
