# ESPHome RTSP Audio

**Status:** Early development. Buggy and unreliable.

External component to stream RTSP audio using ESPHome.

## Goals

- Stream microphone audio off an ESP32 over standard RTSP
- Simple: Single stream, fixed 16 kHz / mono / 16-bit audio, UDP RTP only
- Pair cleanly with ESPHome's mic stack

## Design philosophy

- Use ESPHome built-ins over custom code whenever possible
- ESP-IDF first platform first sine this is the future of ESPHome