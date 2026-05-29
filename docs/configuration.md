# Configuration

This component is configured under `rtsp_audio:` in your ESPHome node YAML.

## Component options

- `microphone:` *(required in practice)*
  - Points at the I²S microphone component you want to stream from
    via a `microphone:` sub-key. The audio shape is locked to
    **32 kHz mono 16-bit PCM** by the schema so samples pass straight
    into RTP with no resampler pulled in — see [Audio format](../README.md#audio-format)
    in the README for why 32 kHz is the sweet spot for the MEMS mics
    this targets. The validator will reject any other `bits_per_sample`
    or `channels` value, so don't set them. `gain_factor` is technically
    accepted but use the [Audio gain](#audio-gain) HA slider instead —
    it's adjustable at runtime and doesn't round at the source.
- `port:` *(optional, default `554`)*
  - RTSP TCP listen port. 554 is the IANA-assigned RTSP port and the
    default for VLC and most NVRs. Use **8554** if your LAN firewall
    treats privileged ports specially. See
    [Port default decision](architecture.md#port-default-of-554).
- `packet_ms:` *(optional, default `20`, range `10..100`)*
  - RTP packetization interval in milliseconds. 20 ms matches what
    most RTP audio implementations use (G.711, Opus, L16). Lower means
    more responsive but heavier per-packet overhead; higher saves
    bandwidth at the cost of latency. See
    [packet_ms decision](architecture.md#packet_ms-default-of-20-ms).
- `id:` *(optional)*
  - Only worth setting if you run more than one `rtsp_audio:` instance
    on the same node — the `number:` / `sensor:` / `binary_sensor:` /
    `text_sensor:` platforms then take `rtsp_audio_id:` to bind to the
    right parent. With a single instance the binding is automatic.

## Validation constraints

From component schema/final validation:

- Platform/framework: **ESP32 + ESP-IDF only**
  (`cv.only_on_esp32` + `cv.only_with_framework(Framework.ESP_IDF)`).
- Source sample rate: **must be 32 kHz**
  (`microphone.final_validate_microphone_source_schema(..., sample_rate=32000)`).
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
Microphone → Ring buffer → DC blocker → Low-cut filter → High-cut filter → Audio gain → [Soft limiter] ─┬─→ L16 byteswap → RTP
                                                                                                        └─→ Peak meter → peak_level_dbfs sensor
```

The **DC blocker** is a 1-pole high-pass at a fixed 5 Hz (well below
anything audible) that strips the MEMS capsule's DC offset before any
other stage sees it. It is always on — not user-configurable — so the
rest of the chain always works on a centred signal even when the
audible cut filters are bypassed. Costs one [MAC](#what-is-a-mac) per
sample.

The **cut filters** come next so the remaining DC/rumble and any
out-of-band hiss don't eat gain-stage headroom. **Gain** sits
immediately before the L16 byteswap so what HA sees on the slider is
exactly what leaves the wire. At default settings (high-cut = 16 kHz,
gain = 0 dB) the high-cut and gain stages are skipped — only the DC
blocker and low-cut do per-sample work.

The **soft limiter** is an opt-in peak limiter with envelope follower.
It sits after gain and reduces the signal level smoothly when the
envelope exceeds the threshold — so sustained loud passages stay near
the threshold ceiling rather than hard-clipping flat at INT16_MAX. See
[Soft limiter](#soft-limiter) below.

The **peak meter** is a read-only tap on the post-gain (and
post-limiter) signal — it samples what is about to be byteswapped and
sent, so it reflects exactly what the listener hears. It does not
modify the audio; the bit-identical default-path guarantee is
preserved. See [Reading `peak_level_dbfs`](#reading-peak_level_dbfs)
for how to use it to set gain.

### Low and High Cut filters

Two complementary **2nd-order Butterworth (biquad)** IIR stages that
let us define the exact audio band we're interested in: a
**low-cut** that strips energy *below* its cutoff, and a
**high-cut** that rolls off energy *above* its cutoff. Each is
exposed as its own Home Assistant number entity and persists across
reboots.

The user-tunable filters sit *downstream* of the always-on DC
blocker (see [Audio processing](#audio-processing)), so their job is
tone shaping — the DC handling is taken care of for them. That
separation also means **either filter can be turned off entirely**
without re-introducing the MEMS DC offset on the wire.

**Bypass conventions.** Both filters use a slider-edge "off" sentinel
so disabling them feels like a natural slider position rather than a
hidden boolean:

- **Low-cut off:** drag the `low_cut_frequency_hz` slider to **0 Hz**
  (anything below 20 Hz disables the stage).
- **High-cut off:** drag the `high_cut_frequency_hz` slider to its
  maximum (**16 kHz** at the default 32 kHz audio = Nyquist).

In either case the always-on DC blocker keeps running, so the signal
that reaches the gain stage is still DC-clean.

The Butterworth shape gives **−12 dB/oct** stopband rolloff with a
maximally-flat passband — twice the rejection slope of a 1-pole IIR
at the same cutoff frequency, with no in-band ripple. At the default
100 Hz low-cut that means 50 Hz rumble is ~12 dB rejected (vs ~6 dB
for the 1-pole shape). See
[docs/images/filter-response.png](images/filter-response.png) for a
combined magnitude plot of the DC blocker (5 Hz), low-cut (100 Hz),
and high-cut (10 kHz) on a single graph.

The biquad is implemented in single-precision float so the same audio
cost lands on **every supported ESP target** — ESP32-S3 uses its FPU,
S2 and C3 use software float (slower per op but still bounded). We
chose this over a faster fixed-point implementation that would have
needed integer-only support on S2 and C3 and FPU on S3, because the
component is meant to ship to all of them without surprises. The
biquad is roughly 5× the [MACs](#what-is-a-mac) of the previous
1-pole, which matters mainly on the no-FPU parts — see the
[CPU-use](#diagnostic-sensors) guidance if you're running close to
the limit.

**When to use the low-cut.** MEMS microphones like the INMP441 ship
with a small DC offset and pick up a lot of low-frequency rumble (HVAC,
handling, wind). That energy wastes dynamic range and makes downstream
stages clip earlier and thump on level changes. The low-cut removes it
at the source so consumers of the RTSP stream (Frigate, BirdNET-Go,
voice pipelines, NVRs) get a cleaner signal to work with. Raise the
cutoff in noisier rooms or for non-voice sources where a more
aggressive cut sounds better.

**When to use the high-cut.** Useful for taming microphone hiss, wind
noise, and out-of-band content that downstream consumers don't need —
e.g. narrow-band voice models or NVR storage where the high
frequencies are wasted bandwidth. It's off by default; dial the slider
down from Home Assistant to engage it.

| Entity | Config key | Default | Range | Step | Notes |
|---|---|---|---|---|---|
| Low cut | `low_cut_frequency_hz` | 100 Hz | **0 Hz (off)** – 500 Hz | 10 Hz | 100 Hz leaves voice fundamentals intact while cutting the bulk of HVAC and handling rumble. Drag to 0 Hz to disable; the DC blocker keeps running. |
| High cut | `high_cut_frequency_hz` | 16000 Hz (off) | 1000 – **16000 Hz (off)** | 100 Hz | At the max the stage is skipped — Nyquist for 32 kHz audio, so there's nothing left to cut anyway. |

```yaml
number:
  - platform: rtsp_audio
    low_cut_frequency_hz:
      name: "Low cut frequency"
    high_cut_frequency_hz:
      name: "High cut frequency"
```

Both entities are tagged `entity_category: config` so HA groups them
under configuration rather than the main controls. If you have **more
than one** `rtsp_audio:` instance, add `rtsp_audio_id: <component-id>`
next to `platform: rtsp_audio` so the entities bind to the right
parent.

### Audio gain

Software audio gain applied after the cut filters and before the
RTP byteswap. Lets you lift the level of a quiet mic, or back it off
for a loud source, without re-flashing or touching the I²S `gain_factor`
(which rounds at the source). The setting persists across reboots.

The slider is in **decibels**, so each step is a uniform perceptual
change. The default is **0 dB** (unity); at exactly 0 dB the gain
stage is skipped entirely and the byte stream is bit-identical to a
build without the gain feature. On overflow the output is
saturating-clamped to the int16 range, so loud passages compress flat
rather than wrapping into scratchy noise.

Internally the audio pipeline still multiplies by a Q8 linear
coefficient — dB is just the unit the HA entity speaks. The device
boot log shows all three representations, e.g.
`Audio gain: +6.0 dB (2.00x, Q8=512)`, for debugging.

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
```

Defaults: `initial_value: 0.0`, `min_value: -20.0`, `max_value: 40.0`,
`step: 1.0`, `restore_value: true`. The entity is tagged
`entity_category: config` so HA groups it with the cut filters under
configuration rather than the main controls.

The +40 dB ceiling (100× linear) is deliberately generous so a very
quiet MEMS mic in a large room can be lifted to a usable level. Above
~+18 dB you'll typically run into the saturating clamp on transients
well before you run out of slider — that's the point at which the
[soft limiter](#soft-limiter) is worth enabling.

The gain, low-cut, and high-cut entities share a single
`platform: rtsp_audio` block — declare them under the same list item:

```yaml
number:
  - platform: rtsp_audio
    low_cut_frequency_hz:
      name: "Low cut frequency"
    high_cut_frequency_hz:
      name: "High cut frequency"
    gain_db:
      name: "Audio gain"
```

### Soft limiter

Post-gain peak limiter with 1-pole IIR envelope follower. Prevents the
gain stage from hard-clipping loud transients: when the signal's
running peak envelope exceeds the threshold, a proportional gain
reduction is applied immediately and released gradually, so sustained
loud passages stay near the threshold ceiling rather than producing
flat-top distortion.

Disabled by default (opt-in, like every other DSP stage). Enable via
the `soft_limiter_enabled` switch in Home Assistant; the switch state
is persisted across reboots.

**When to use it.** At gain settings above ~+18 dB, transient peaks
will occasionally hit the hard saturating clamp in the gain stage.
The soft limiter sits just below that ceiling and smoothly reduces gain
before the clamp fires, so the output sounds compressed rather than
clipped. Enable the `limiter_gain_reduction_db` sensor to see how much
the limiter is working; cross-reference with `peak_level_dbfs` to
confirm peaks are staying near (not above) the threshold.

**Parameter guide.**

| Entity | Config key | Default | Range | Step | Notes |
|---|---|---|---|---|---|
| Enable | `soft_limiter_enabled` | off | — | — | Toggle in HA; persisted. Off = skipped, bit-identical to a build without this stage. |
| Threshold | `soft_limiter_threshold_db` | −3 dBFS | −20 to 0 dBFS | 1 dB | The ceiling the limiter defends. Lower = more conservative (tighter ceiling, more audible compression). −3 dBFS gives comfortable headroom below the hard INT16 clip. |
| Attack | `soft_limiter_attack_ms` | 5 ms | 0.1 to 50 ms | 0.1 ms | How quickly the envelope follower latches onto a rising peak. Shorter = catches fast transients more aggressively but can sound "pumpy". Longer = gentler onset, brief spikes may still clip. |
| Release | `soft_limiter_release_ms` | 100 ms | 10 to 2000 ms | 10 ms | How quickly the gain reduction recovers after a loud passage. Shorter = faster recovery, potentially audible "pumping". Longer = smoother but keeps gain depressed longer between sentences. |

```yaml
number:
  - platform: rtsp_audio
    soft_limiter_threshold_db:
      name: "Soft limiter threshold"
    soft_limiter_attack_ms:
      name: "Soft limiter attack"
    soft_limiter_release_ms:
      name: "Soft limiter release"

switch:
  - platform: rtsp_audio
    soft_limiter_enabled:
      name: "Soft limiter"
```

The three number entities can be declared in the same `platform: rtsp_audio`
block as the cut filters and gain slider — they share the same schema
block. The switch goes in its own `switch:` block.

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
| `peak_level_dbfs` | sensor | Peak absolute sample value across the last window, expressed in dBFS (0 dBFS = full-scale clipping). Tapped **post-gain** so it shows what is actually streamed. Silent windows publish a `-100 dBFS` floor; on session close the floor is also published, matching how `cpu_use_pct` publishes 0 at rest. | Once per 5 s while streaming. |
| `limiter_gain_reduction_db` | sensor | Maximum gain reduction applied by the soft limiter in the last window, in dB (0 = no limiting active, 3 = 3 dB of reduction). Useful for confirming the limiter is engaging and by how much. Publishes 0 when the limiter is bypassed or when no reduction occurred; publishes 0 on session close. | Once per 5 s while streaming. |

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
    peak_level_dbfs:
      name: "Peak level"
    limiter_gain_reduction_db:
      name: "Limiter gain reduction"
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
| **70 – 90 %** | Running hot. | Brief audio glitches may appear when Wi-Fi gets busy or other tasks spike. Consider simplifying the DSP: drop the high-cut filter (set it to its max) — the biquad costs about 5× the MACs of the previous 1-pole, so disabling it claws back the most CPU. Lower the gain to take the bit-identical unity path too. |
| **> 90 % sustained** | Chip is at its limit. | Expect audio dropouts. Upgrade to a more capable board (e.g. ESP32-S2 → dual-core ESP32-S3) or disable features. |
| **100 %** | Out of budget; audio will glitch. | Hard "upgrade the chip or turn things off" signal. |

At the default 32 kHz mono 16-bit configuration with the default DSP
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

#### Reading `peak_level_dbfs`

The sensor exists to answer the practical question **"is the gain set
right?"** Too high and the loudest peaks clip; too low and the stream
lives in the noise floor and downstream consumers have to re-amplify
along with whatever noise the mic picked up. The tap sits **after**
the gain stage (see the [block diagram](#audio-processing)), so 0 dBFS
on the meter means the gain stage is clipping — measuring pre-gain
would hide exactly the failure mode this sensor is meant to surface.
Values are the largest absolute sample seen in each 5 s window,
rounded to whole dB, with a `-100 dBFS` floor for silence.

Move the `Audio gain` slider and aim for peaks at **-6 to -3 dBFS**:

| Reading | What it means | Action |
|---|---|---|
| **`-100 dBFS` (floor)** | No client streaming, or true silence in the window. | Expected when idle. While streaming, suggests the mic isn't picking anything up. |
| **< -40 dBFS** | Under-driven — most of the int16 range is unused. | Raise the gain slider; downstream consumers are working with a tiny fraction of the available dynamic range. |
| **-40 to -20 dBFS** | Quiet but usable. | Fine for very quiet sources; raise gain if the typical content is louder. |
| **-20 to -6 dBFS** | Healthy working range. | Leave the gain alone. |
| **-6 to -1 dBFS** | Approaching clip. | Acceptable for a worst-case peak, but keep an eye on it. Back gain off a few dB if peaks hit -1 often. |
| **0 dBFS** | Clipping. | The gain stage is saturating loud passages flat. Reduce the gain slider until peaks settle below 0. |

At the default 0 dB gain a typical room-level voice source reads in
the -30 to -10 dBFS range; the exact value depends entirely on mic
sensitivity, distance, and source loudness, which is why the meter
exists in the first place.

##### Slowing the meter down for long-term monitoring

The 5 s cadence is tuned for setting gain interactively — fast enough
that the meter responds to slider moves without making you wait. If
you want a quieter signal for a long-term level-history graph in HA,
layer an ESPHome filter on top rather than asking the component to
publish less often (fast → slow is one filter away; slow → fast would
need a firmware change). A `max` filter holds the loudest reading over
its window, which is what you want here — averaging would smear a
brief -3 dBFS clip together with the surrounding -50 dBFS silence into
a meaningless -26 dBFS, masking exactly the event you care about. Each
`window_size` unit is one 5 s publish, so `12` gives a 1-minute peak
hold:

```yaml
sensor:
  - platform: rtsp_audio
    peak_level_dbfs:
      name: "Peak level (1 min)"
      filters:
        - max:
            window_size: 12
            send_every: 12
            send_first_at: 12
```

If you'd rather keep the ESPHome side simple and shape the graph in
HA, the same effect is reachable with HA's
[`statistics`](https://www.home-assistant.io/integrations/statistics/),
[`min_max`](https://www.home-assistant.io/integrations/min_max/), or
[`filter`](https://www.home-assistant.io/integrations/filter/)
integrations on the entity.

#### Reading `limiter_gain_reduction_db`

The sensor answers the practical question **"is the limiter working, and is it working too hard?"**

- **0 dB** — no gain reduction in the last 5 s window. Either the limiter is off, or it's on but all signals were below the threshold.
- **1–3 dB** — light limiting, occasional peaks catching the threshold. Normal healthy operation when the gain is set aggressively.
- **> 6 dB** — sustained heavy limiting. The limiter is actively compressing most of the signal energy. Either the gain is too high for the source level (back the `gain_db` slider off) or the threshold is too low (raise `soft_limiter_threshold_db` toward 0 dBFS).

The sensor reports the **maximum** reduction seen in each 5 s window, so even a brief loud event that triggered 6 dB of gain reduction will show as 6 dB even if the rest of the window was quiet. Cross-reference with `peak_level_dbfs`: if that reads near the threshold and this reads a few dB, the limiter is doing its job correctly. If `peak_level_dbfs` is still 0 dBFS while this shows large reduction, the limiter is arriving too late (try a shorter attack time).

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

## Interesting further reading

### Why is the cutoff frequency at −3 dB?

When a filter spec says "100 Hz cutoff," that means the filter is at
**−3 dB at 100 Hz** — not that it suddenly blocks everything below
that frequency. Two reasons that specific point became the convention:

**It's the half-power point.** −3 dB means the signal amplitude has
dropped to 1/√2 ≈ 0.707 of the passband level. Since power is
proportional to amplitude², that is exactly half the original power
still getting through. "Starts rolling off" is a fuzzy idea; "half
power" is a clean, physically meaningful line to draw.

**It falls out of the math naturally.** The simplest analog filter —
a first-order RC circuit — has transfer function
`H(f) = 1 / (1 + j·f/fc)`, where `fc = 1/(2πRC)`. Evaluating that
at `f = fc` gives `|H| = 1/√2`, i.e. exactly −3 dB. The bilinear
transform used to convert analog filter designs to digital preserves
that point. So −3 dB at `fc` isn't an arbitrary convention bolted on
later — it is where the pole of the original analog prototype
naturally sits.

All the filters in this component define their cutoff at the −3 dB
point: the DC blocker at 5 Hz, the low-cut at whatever you set it to,
and the high-cut likewise. The frequency response graph in
[docs/images/filter-response.png](images/filter-response.png) shows
the dotted −3 dB reference line crossing each filter curve exactly at
its stated cutoff frequency.

### What is a MAC?

"MAC" stands for **multiply-accumulate**: the single fused operation
`y += a * x` that sits at the heart of every IIR filter, FIR filter,
FFT butterfly, and matrix multiply you've ever read about. It's also
the smallest useful unit of DSP work that a CPU can do, which is why
chip vendors quote MIPS in *MMAC/s* and DSP textbooks measure filter
cost in *MACs per sample*.

When this doc says "the biquad is roughly 5× the MACs of the
previous 1-pole," it means: a 2nd-order biquad does five
multiply-and-add operations per audio sample (`b0*x + b1*x[-1] +
b2*x[-2] − a1*y[-1] − a2*y[-2]`) where the old 1-pole did one
(`y = x − x[-1] + R*y[-1]` collapses to a single multiply +
add/sub on each step). At 32 kHz mono, that's 32000 × 5 = 160k MACs
per second per biquad — trivial on a chip with an FPU, more
expensive on the no-FPU parts where each MAC is a software-float
call.

You'll see the term used a few places in this doc and in the
component comments. The point is to give you a unit that's
independent of the specific MCU: filter A costing "5 MACs per
sample" tells you it's about 5× the work of filter B at "1 MAC per
sample," whether you're on an S3 with an FPU or an S2 without one.

### Why we byteswap before sending

The block diagram in [Audio processing](#audio-processing) has an
`L16 byteswap` stage right before RTP. It exists to bridge two
opposite conventions:

- **The ESP32 is little-endian.** Each 16-bit PCM sample sits in
  memory low byte first. That's also how the I²S driver and the
  ring buffer hand samples to us — no conversion has happened yet.
- **RTP L16 is big-endian.** RFC 3551 §4.5.11 defines the `L16`
  payload as "two's complement 16-bit samples in network byte
  order." Network byte order is big-endian, same rule as every
  other field in the RTP/IP/TCP/UDP headers.

So without a swap, a sample that should sound like `0x1234` arrives
on the wire as `0x3412`, and the receiver interprets it as a
completely different (and usually much louder, sign-flipped) value.
The audible result is loud white-noise-like garbage, not a quieter
or distorted version of the original — the high and low bytes of
every sample are transposed.

We do the swap in place with `esphome::convert_big_endian()` on the
payload region of the RTP packet we're about to send, so there's no
second buffer involved. It's the last thing that touches the samples
before `sendto()` / the TCP-interleaved write, which is why it
appears at the end of the processing chain.

For the same reason, the 16- and 32-bit fields of the RTP header
(sequence number, timestamp, SSRC) also go through
`convert_big_endian()` on the way out. Using one helper for both the
header fields and the audio samples is what lets us avoid carrying
our own `store_u16_be` / `store_u32_be` helpers — see
[Implementation wins](architecture.md#implementation-wins) in the
architecture notes.
