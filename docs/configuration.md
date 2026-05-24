# Configuration

This component is configured under `rtsp_audio:` in your ESPHome node YAML.

## Component options

- `id` *(optional, ID)*
  - ESPHome component ID.
- `microphone` *(optional block, defaults to `{}` but effectively required)*
  - Microphone source block created with `microphone_source_schema`.
  - Sub-options:
    - `microphone` *(required)*: ID of the microphone component to consume.
    - `bits_per_sample` *(optional, default `16`)*: constrained to `16` for this MVP.
    - `channels` *(optional, default `[0]`)*: constrained to mono for this MVP.
    - `gain_factor` *(optional, default `1`)*: software gain applied by `MicrophoneSource`.
  - Why this shape: the same 16 kHz mono 16-bit PCM that `voice_assistant`
    standardised on, picked so we can pass mic samples straight to RTP
    without resampling. See
    [Audio shape decision](architecture.md#audio-shape-16-khz-mono-16-bit-pcm).
- `port` *(optional, default `554`)*
  - RTSP TCP listen port. 554 is the IANA-assigned RTSP port and the
    default for VLC and most NVRs. Use **8554** if your LAN firewall
    treats privileged ports specially. See
    [Port default decision](architecture.md#port-default-of-554).
- `packet_ms` *(optional, default `20`, range `10..100`)*
  - RTP packetization interval in milliseconds. 20 ms matches what most
    RTP audio implementations use (G.711, Opus, L16). Lower means more
    responsive but heavier per-packet overhead; higher saves bandwidth
    at the cost of latency. See
    [packet_ms decision](architecture.md#packet_ms-default-of-20-ms).

## Validation constraints

From component schema/final validation:

- Platform/framework: **ESP32 + ESP-IDF only**
  (`cv.only_on_esp32` + `cv.only_with_framework(Framework.ESP_IDF)`).
- Source sample rate: **must be 16 kHz**
  (`microphone.final_validate_microphone_source_schema(..., sample_rate=16000)`).
- Source audio shape: **mono, 16-bit** after microphone source processing
  (`min/max_bits_per_sample=16`, `min/max_channels=1`).
- Session model: **single client**
  (additional connections are rejected at TCP `accept()`).
- RTP transport: **UDP (`RTP/AVP`) or TCP-interleaved (`RTP/AVP/TCP`)**,
  chosen per client at `SETUP` — no configuration needed. The UDP media
  path is IPv4-only; TCP-interleaved also works over IPv6.

The reasoning behind each constraint lives in
[`docs/architecture.md`](architecture.md#key-design-decisions).

## Session behavior

The server accepts one RTSP client at a time. A second connection is
rejected at `accept()` until the first session ends.

A session ends in one of three ways:

1. The client sends `TEARDOWN`, or closes the TCP control connection.
2. Wi-Fi drops on the device side (`network::is_connected()` goes false).
3. **Inactivity timeout** — the server closes the session if no RTSP
   request arrives for **60 seconds** (the same value it advertises in
   SETUP's `Session: <id>;timeout=60` field). This covers the case
   where a client crashes, sleeps, or loses its own Wi-Fi without
   sending `TEARDOWN`: without it, the half-open TCP connection would
   block new clients until the OS-level TCP timeout fires (usually
   minutes).

What counts as activity: any RTSP request on the control channel —
`OPTIONS`, `GET_PARAMETER`, `DESCRIBE`, `SETUP`, `PLAY`, `PAUSE`,
`TEARDOWN`. Conformant clients (VLC, ffplay, most NVRs) send periodic
`OPTIONS` or `GET_PARAMETER` keep-alives during PLAY for exactly this
reason. **The outgoing RTP audio stream does not count** — the RTSP
spec ties the keep-alive contract to the control channel, so a player
that streams RTP but never sends a keep-alive will be reaped after 60 s.

User-visible log line when the timeout fires:

```
[W][rtsp_audio]: RTSP session idle > 60s; closing
[I][rtsp_audio]: RTSP session closed
```

The timeout is a compile-time constant (`SESSION_TIMEOUT_SECONDS` in
`rtsp_audio.h`); it is not currently exposed as a YAML option.

## Audio processing

Stages run per sample inside the RTP send loop, in this order:

```
Microphone → Ring buffer → Low-cut filter → Input gain → L16 byteswap → RTP
```

The low-cut runs first so DC and rumble don't eat headroom before the
gain stage. Gain sits immediately before the L16 byteswap so what HA
sees on the slider is exactly what leaves the wire. At default settings
(gain = 1.0) the gain stage is skipped and the byte stream is
bit-identical to a build without the gain feature.

### Low-cut filter

MEMS microphones like the INMP441 ship with a small DC offset and
pick up a lot of low-frequency rumble (HVAC, handling, wind). That
energy wastes dynamic range and makes downstream stages clip earlier
and thump on level changes. The low-cut filter removes it at the
source so consumers of the RTSP stream (Frigate, BirdNET-Go, voice
pipelines, NVRs) get a cleaner signal to work with.

The filter is always on. The cut frequency defaults to **100 Hz**,
which is low enough to leave voice fundamentals intact while still
cutting the bulk of HVAC and handling rumble. You can tune it from
Home Assistant by adding the optional `number` block below —
useful for noisier rooms or non-voice sources where a more
aggressive cut sounds better. The setting persists across reboots.

```yaml
number:
  - platform: rtsp_audio
    lowcut_filter_frequency:
      name: "RTSP Low Cut Filter Frequency"
      unit_of_measurement: "Hz"
```

Defaults: `initial_value: 100`, `min_value: 20`, `max_value: 500`,
`step: 10`, `restore_value: true`. Values are in Hz. The entity is
tagged `entity_category: config` so HA groups it under configuration
rather than the main controls.

If you have **more than one** `rtsp_audio:` instance, add
`rtsp_audio_id: <component-id>` next to `platform: rtsp_audio` so the
entity binds to the right parent.

### Input gain

Software input gain applied after the low-cut filter and before the
RTP byteswap. Lets you lift the level of a quiet mic, or back it off
for a loud source, without re-flashing or touching the I²S `gain_factor`
(which rounds at the source). The setting persists across reboots.

The default is **1.0** (unity), and at exactly 1.0 the gain stage is
skipped entirely — the byte stream is bit-identical to a build without
the gain feature. On overflow the output is saturating-clamped to the
int16 range, so loud passages compress flat rather than wrapping into
scratchy noise. (For a soft knee instead of hard saturation, see the
"Soft limiter" item on the roadmap.)

> **Note:** the value is a **linear multiplier**, not dB. `2.0` is
> twice the amplitude (≈ +6 dB), `0.5` is half (≈ −6 dB). A dB display
> for HA is on the todo list.

```yaml
number:
  - platform: rtsp_audio
    gain:
      name: "Audio gain"
```

Defaults: `initial_value: 1.0`, `min_value: 0.1`, `max_value: 80.0`,
`step: 0.1`, `restore_value: true`. The entity is tagged
`entity_category: config` so HA groups it with the low-cut filter under
configuration rather than the main controls.

The 80× ceiling is deliberately generous so a very quiet MEMS mic in a
large room can be lifted to a usable level. Past roughly 8× you'll
typically run into the saturating clamp on transients well before you
run out of slider — that's the point at which the planned soft limiter
becomes worth wiring in.

The gain and low-cut entities share a single `platform: rtsp_audio`
block — declare them under the same list item:

```yaml
number:
  - platform: rtsp_audio
    lowcut_filter_frequency:
      name: "RTSP Low Cut Filter Frequency"
      unit_of_measurement: "Hz"
    gain:
      name: "Audio gain"
```

## Diagnostic sensors

A small, opinionated set of stream state is exposed to Home Assistant
via standard ESPHome sensor platforms. All three are **opt-in** — the
component compiles them out unless you reference it from a
`binary_sensor:` / `text_sensor:` / `sensor:` block.

| Entity | Platform | What it shows | Updates |
|---|---|---|---|
| `client_connected` | binary_sensor | `on` while an RTSP session is active (between `SETUP` and `TEARDOWN` / network loss / 60 s idle timeout). | Edge-triggered at each session boundary. |
| `client_ip` | text_sensor | IP address of the currently connected client, empty when none. | Edge-triggered at each session boundary. |
| `bytes_sent` | sensor | Cumulative RTP payload bytes sent in the **current** session. Resets to 0 on each new `PLAY` and on session close. | Once per 5 s while streaming. |

All three are tagged `entity_category: diagnostic` so HA groups them on
the device's diagnostics card rather than the main controls.

The scope is intentionally narrow — these three answer the common
questions ("is anything connected? who? is audio actually flowing?")
without flooding HA with internal counters. If you want more, open an
issue.

### Example

```yaml
binary_sensor:
  - platform: rtsp_audio
    client_connected:
      name: "RTSP client connected"

text_sensor:
  - platform: rtsp_audio
    client_ip:
      name: "RTSP client IP"

sensor:
  - platform: rtsp_audio
    bytes_sent:
      name: "RTSP bytes sent"
```

If you have **more than one** `rtsp_audio:` instance, add
`rtsp_audio_id: <component-id>` next to `platform: rtsp_audio` so each
sensor binds to the right parent. With a single instance the binding is
resolved automatically.

`bytes_sent` is the raw RTP payload byte count and is a monotonic
counter within a session, so it pairs well with HA's
[`derivative` sensor](https://www.home-assistant.io/integrations/derivative/)
or [Riemann sum](https://www.home-assistant.io/integrations/integration/)
to derive a bitrate.

## Minimal YAML block

```yaml
rtsp_audio:
  id: rtsp_mic
  microphone:
    microphone: mic_inmp441
  port: 554
  packet_ms: 20
```

## Full example context

See [`example_rtsp_audio.yaml`](../example_rtsp_audio.yaml) for a complete node definition including:

- `esp32` / `framework: esp-idf`
- `i2s_audio` pin setup
- `microphone` (`platform: i2s_audio`)
- `external_components` local source
- `rtsp_audio` block
