# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog 1.1.0](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning 2.0.0](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **Soft limiter.** Opt-in post-gain peak limiter with 1-pole IIR
  envelope follower. Addresses the hard saturating clamp that fires on
  transients above ~+18 dB gain: instead of producing flat-top
  distortion, the limiter applies proportional gain reduction once the
  running peak envelope exceeds the threshold, and releases smoothly
  after the transient passes. Controlled via a new `switch:` entity
  (`soft_limiter_enabled`, disabled by default) plus three `number:`
  entities in the existing `platform: rtsp_audio` block:
  `soft_limiter_threshold_db` (−3 dBFS default, −20–0 dBFS),
  `soft_limiter_attack_ms` (5 ms default, 0.1–50 ms), and
  `soft_limiter_release_ms` (100 ms default, 10–2000 ms). All four
  entities are persisted across reboots. The stage runs after the gain
  stage and before the peak meter, so the `peak_level_dbfs` sensor
  reflects limited output. With the switch off the stage is completely
  bypassed — no CPU cost and bit-identical output. See
  [docs/configuration.md#soft-limiter](docs/configuration.md#soft-limiter).
- **Soft limiter gain-reduction sensor (`limiter_gain_reduction_db`).** Opt-in
  `sensor:` on `platform: rtsp_audio` reporting the maximum gain reduction
  applied by the soft limiter in each 5 s window, in dB (0 = no limiting
  active, positive = limiting engaged). Follows the same publish-on-change
  pattern as `peak_level_dbfs`: resets to 0 on session close, publishes 0
  when the limiter is bypassed. See
  [docs/configuration.md#reading-limiter_gain_reduction_db](docs/configuration.md#reading-limiter_gain_reduction_db).

## [0.2.0] - 2026-05-29

### Fixed

- **INMP441 I²S aliasing when `bits_per_sample: 16bit` is set
  ([#7](https://github.com/hendrikvh/ESPHome-RTSP-Audio/issues/7)).**
  Setting `bits_per_sample: 16bit` on the `microphone:` block causes
  the I²S peripheral to misalign the 24-bit INMP441 data word, halving
  the effective sample rate and aliasing all energy above 8 kHz back
  onto the lower half of the spectrum. The fix is to use
  `bits_per_sample: 32bit` — the 32-bit frame keeps the word correctly
  aligned; the 8 LSBs are padding zeros and the driver handles them
  correctly.

### Added

- **DC blocker.** A dedicated 1-pole high-pass at a fixed 5 Hz now
  sits upstream of every other DSP stage and runs by default on every
  packet — its only job is to kill the DC offset MEMS capsules
  (INMP441 and friends) ship with, before it eats headroom in any of
  the downstream stages. **Separates concerns cleanly:** the DC
  blocker is hygiene; the user-tunable low-cut is tone shaping. One
  MAC per sample, sub-audible cutoff. **Note:** the default install
  is **no longer bit-identical to v0.1** — the DC blocker runs by
  default. The audible change is the removal of MEMS DC bias, which
  is desirable on every device the component targets.
- **Low-cut bypass at 0 Hz.** The low-cut slider now ranges from
  **0 Hz to 500 Hz**, with anything below the existing active
  minimum (20 Hz) disabling the stage entirely — mirroring how
  16 kHz (Nyquist) already disables the high-cut. Dragging the
  slider to **0** is the natural "off" position. The always-on DC
  blocker keeps running in either case, so disabling the audible
  low-cut never re-introduces DC bias downstream.

### Changed

- **Cut filters upgraded to 2nd-order Butterworth.** Both the low-cut
  and high-cut stages now use a 2nd-order Butterworth (biquad) shape
  instead of the previous 1-pole IIR, doubling the rolloff from
  **−6 dB/oct to −12 dB/oct**. At the default 100 Hz low-cut, 50 Hz
  rumble is now ~12 dB rejected (vs ~6 dB), so HVAC and handling noise
  drop noticeably more without moving the cutoff up into the voice
  band. The Butterworth's defining feature — a **maximally-flat
  passband** — also means voice timbre is preserved cleanly instead
  of being gently tilted by the 1-pole's roll-in starting an octave
  above the cutoff. The −3 dB cutoff frequency convention is unchanged,
  so any persisted Home Assistant slider value keeps the same audible
  meaning; the slope at the cutoff is the only thing that changes.
  Implementation uses single-precision float in a direct-form-I
  biquad — the same audio cost lands on every supported MCU
  (ESP32-S3 uses its FPU, S2 and C3 use software float) rather than
  depending on FPU-only fixed-point math that would have to choose one
  target. User-facing config (cutoff ranges, defaults,
  bypass-at-Nyquist behaviour) is unchanged. See
  [docs/images/filter-response.png](docs/images/filter-response.png)
  for a side-by-side magnitude plot, regenerable via
  `scripts/plot_filter_response.py`.

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

[Unreleased]: https://github.com/hendrikvh/ESPHome-RTSP-Audio/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/hendrikvh/ESPHome-RTSP-Audio/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/hendrikvh/ESPHome-RTSP-Audio/compare/v0.0.1...v0.1.0
[0.0.1]: https://github.com/hendrikvh/ESPHome-RTSP-Audio/releases/tag/v0.0.1
