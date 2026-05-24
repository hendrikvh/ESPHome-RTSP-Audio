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
Microphone → Ring buffer → Low-cut filter → High-cut filter → Input gain → L16 byteswap → RTP
```

The low-cut runs first so DC and rumble don't eat headroom before the
gain stage. The high-cut runs next so any out-of-band hiss is removed
before amplification. Gain sits immediately before the L16 byteswap so
what HA sees on the slider is exactly what leaves the wire. At default
settings (high-cut = 20 kHz, gain = 0 dB) both the high-cut and gain
stages are skipped and the byte stream is bit-identical to a build
without those features.

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

### High-cut filter

The complementary stage to the low-cut: a one-pole IIR low-pass that
rolls off energy **above** the cutoff. Useful for taming microphone
hiss, wind noise, and out-of-band content that downstream consumers
don't need — e.g. narrow-band voice models or NVR storage where the
high frequencies are wasted bandwidth.

The filter is **off by default** (cutoff = 20 kHz, which is above the
16 kHz audio's Nyquist frequency). At the default the stage is skipped
entirely and the byte stream is bit-identical to a build without the
high-cut feature. Dial the slider down from Home Assistant to engage
it; the setting persists across reboots.

```yaml
number:
  - platform: rtsp_audio
    highcut_filter_frequency:
      name: "RTSP High Cut Filter Frequency"
      unit_of_measurement: "Hz"
```

Defaults: `initial_value: 20000`, `min_value: 1000`, `max_value: 20000`,
`step: 100`, `restore_value: true`. Values are in Hz; the max (20 kHz)
is the "filter off" position. The entity is tagged
`entity_category: config` so HA groups it with the low-cut and gain
controls under configuration rather than the main controls.

If you have **more than one** `rtsp_audio:` instance, add
`rtsp_audio_id: <component-id>` next to `platform: rtsp_audio` so the
entity binds to the right parent.

### Input gain

Software input gain applied after the low-cut filter and before the
RTP byteswap. Lets you lift the level of a quiet mic, or back it off
for a loud source, without re-flashing or touching the I²S `gain_factor`
(which rounds at the source). The setting persists across reboots.

The slider is in **decibels**, so each step is a uniform perceptual
change. The default is **0 dB** (unity); at exactly 0 dB the gain
stage is skipped entirely and the byte stream is bit-identical to a
build without the gain feature. On overflow the output is
saturating-clamped to the int16 range, so loud passages compress flat
rather than wrapping into scratchy noise. (For a soft knee instead of
hard saturation, see the "Soft limiter" item on the roadmap.)

Internally the audio pipeline still multiplies by a Q8 linear
coefficient — dB is just the unit the HA entity speaks. The device
boot log shows all three representations, e.g.
`Input gain: +6.0 dB (2.00x, Q8=512)`, for debugging.

| Slider | Linear |
|---|---|
| −20 dB | 0.1× |
| −6 dB | ≈ 0.5× |
| 0 dB | 1.0× (bit-identical fast path) |
| +6 dB | ≈ 2.0× |
| +20 dB | 10× |
| +40 dB | 100× |

```yaml
number:
  - platform: rtsp_audio
    gain_db:
      name: "Audio gain"
      unit_of_measurement: "dB"
```

Defaults: `initial_value: 0.0`, `min_value: -20.0`, `max_value: 40.0`,
`step: 1.0`, `restore_value: true`. The entity is tagged
`entity_category: config` so HA groups it with the low-cut filter under
configuration rather than the main controls.

The +40 dB ceiling (100× linear) is deliberately generous so a very
quiet MEMS mic in a large room can be lifted to a usable level. Above
~+18 dB you'll typically run into the saturating clamp on transients
well before you run out of slider — that's the point at which the
planned soft limiter becomes worth wiring in.

The gain, low-cut, and high-cut entities share a single
`platform: rtsp_audio` block — declare them under the same list item:

```yaml
number:
  - platform: rtsp_audio
    lowcut_filter_frequency:
      name: "RTSP Low Cut Filter Frequency"
      unit_of_measurement: "Hz"
    highcut_filter_frequency:
      name: "RTSP High Cut Filter Frequency"
      unit_of_measurement: "Hz"
    gain_db:
      name: "Audio gain"
      unit_of_measurement: "dB"
```

## Diagnostic sensors

A small, opinionated set of stream state is exposed to Home Assistant
via standard ESPHome sensor platforms. All entries are **opt-in** — the
component compiles them out unless you reference it from a
`binary_sensor:` / `text_sensor:` / `sensor:` block.

| Entity | Platform | What it shows | Updates |
|---|---|---|---|
| `client_connected` | binary_sensor | `on` while an RTSP session is active (between `SETUP` and `TEARDOWN` / network loss / 60 s idle timeout). | Edge-triggered at each session boundary. |
| `client_ip` | text_sensor | IP address of the currently connected client, empty when none. | Edge-triggered at each session boundary. |
| `bytes_sent` | sensor | Cumulative RTP payload bytes sent in the **current** session. Resets to 0 on each new `PLAY` and on session close. | Once per 5 s while streaming. |
| `cpu_use_pct` | sensor | Percentage of wall-clock that the RTSP audio path (component `loop()` body + mic data callback) consumed in the last window. Range 0–100 %, one decimal. Resets at each `PLAY`; published as 0 on session close. | Once per ~10 s while streaming. |

All entries are tagged `entity_category: diagnostic` so HA groups them on
the device's diagnostics card rather than the main controls.

The scope is intentionally narrow — these answer the common
questions ("is anything connected? who? is audio actually flowing? do
I have CPU headroom?") without flooding HA with internal counters. If
you want more, open an issue.

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
    cpu_use_pct:
      name: "CPU use"
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

#### Reading `cpu_use_pct`

The sensor exists to answer the practical question **"is this ESP chip
up to the task, or do I need a more powerful one / do I need to disable
a feature?"** Use these threshold bands as guidance (the exact numbers
depend on what else the device is doing):

| Reading | What it means | Action |
|---|---|---|
| **< 30 %** | Plenty of headroom. | Safe to add features (more aggressive DSP, future stereo, higher sample rate if it becomes configurable). |
| **30 – 70 %** | Normal working range. | Audio is fine. Adding features will eat into the remaining budget. |
| **70 – 90 %** | Running hot. | Brief audio glitches may appear when Wi-Fi gets busy or other tasks spike. Consider simplifying the DSP: raise the low-cut to skip the IIR work, drop the high-cut filter (set it to its max), lower the gain to take the bit-identical unity path. |
| **> 90 % sustained** | Chip is at its limit. | Expect audio dropouts. Upgrade to a more capable board (e.g. ESP32-S2 → dual-core ESP32-S3) or disable features. |
| **100 %** | Out of budget; audio will glitch. | Hard "upgrade the chip or turn things off" signal. |

At the default 16 kHz mono 16-bit configuration with the default DSP
(low-cut at 100 Hz, high-cut off, gain at 0 dB) the value typically sits
in the low single digits, so anything above that is your own feature
configuration showing up in the measurement.

The metric is **self-measured** — it counts only the time spent inside
the `rtsp_audio` component's `loop()` body and mic data callback. It
does **not** include Wi-Fi / LwIP work (which executes in its own task),
time the I²S driver spends DMA-handling before the callback fires, or
work done by other ESPHome components. Treat the value as a lower bound
on total system load: a low reading doesn't prove the whole system is
idle, but a high reading reliably means the audio path is the
bottleneck. The measurement itself adds well under 0.1 % of CPU, so it
doesn't meaningfully bias the value it reports.

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
