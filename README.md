# ESPHome RTSP Audio

**Status:** Early development. Buggy and unreliable.

External component to stream RTSP audio using ESPHome.

## Features

- **RTP over UDP** (`RTP/AVP`) — the classic transport. works with VLC and most media players.
- **RTP over TCP** (`RTP/AVP/TCP`, interleaved) — RTP framed on the RTSP connection. Works with FFmpeg, BirdNET-Go, Frigate, and most NVRs.
- Transport is negotiated per client at `SETUP`, so UDP and TCP clients both work with no configuration.
- Uncompressed **L16 PCM** audio — 16 kHz mono 16-bit, RTP payload type 96 (`L16/16000/1`).
- One client at a time.

## Hardware

A **dual-core ESP32 is recommended, e.g. the ESP32-S3.** On single-core chips
(such as the ESP32-S2 that I'm testing on) the Wi-Fi stack and the audio loop 
share one CPU core, so occasional brief (~1 s) audio gaps occur when Wi-Fi gets
busy. A dual-core chip runs Wi-Fi and the audio loop on separate cores and avoids this.

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