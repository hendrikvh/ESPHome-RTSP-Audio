#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "audio_pipeline.h"
#include "dc_blocker.h"
#include "gain.h"
#include "high_cut.h"

namespace esphome::rtsp_audio::internal {
namespace {

// Reference implementation of what the pre-gain build did: DC-blocker
// then byteswap. Used to confirm the defaults fast path is
// bit-identical to the legacy loop.
void reference_unity_pipeline(std::vector<int16_t> &samples, DcBlockerState &s, int32_t r_q15) {
  for (auto &sample : samples) {
    const int16_t filtered = dc_blocker_step(sample, s, r_q15);
    sample = static_cast<int16_t>(byteswap_u16(static_cast<uint16_t>(filtered)));
  }
}

TEST(AudioPipeline, DefaultsMatchReference) {
  // Same input, two state machines, two paths through the code. With
  // high-cut OFF and unity gain, the pipeline's fast-path must produce
  // byte-for-byte the same payload as the pre-gain implementation —
  // that's what existing RTSP consumers have been receiving.
  std::vector<int16_t> input{0, 1000, -1000, 4242, -4242, 30000, -30000, 12345};
  std::vector<int16_t> ref = input;
  std::vector<int16_t> got = input;
  DcBlockerState s_ref{};
  DcBlockerState s_got{};
  HighCutState hc_got{};

  reference_unity_pipeline(ref, s_ref, DC_BLOCKER_DEFAULT_R_Q15);
  process_l16_payload_inplace(got.data(), got.size(), s_got, DC_BLOCKER_DEFAULT_R_Q15, hc_got, HIGH_CUT_A_Q15_OFF,
                              GAIN_Q8_UNITY);

  EXPECT_EQ(ref, got);
}

TEST(AudioPipeline, ScaledPathSaturatesAtMaxGain) {
  // Loud input + max gain → output must clamp to INT16_MIN/MAX (in big
  // endian) and never wrap. High-cut OFF so the result is comparable to
  // the original pre-high-cut behavior.
  std::vector<int16_t> samples{20000, -20000, 20000, -20000};
  DcBlockerState s{};
  HighCutState hc{};
  process_l16_payload_inplace(samples.data(), samples.size(), s, DC_BLOCKER_DEFAULT_R_Q15, hc, HIGH_CUT_A_Q15_OFF,
                              gain_q8_for(GAIN_MAX));

  for (int16_t v : samples) {
    const int16_t native = static_cast<int16_t>(byteswap_u16(static_cast<uint16_t>(v)));
    EXPECT_TRUE(native == INT16_MAX || native == INT16_MIN) << "got " << native;
  }
}

TEST(AudioPipeline, ZeroInputZeroOutput) {
  // Silence in → silence out, regardless of gain or whether high-cut is on.
  std::vector<int16_t> samples(64, 0);
  DcBlockerState s{};
  HighCutState hc{};
  process_l16_payload_inplace(samples.data(), samples.size(), s, DC_BLOCKER_DEFAULT_R_Q15, hc,
                              high_cut_a_q15_for(2000.0f, 32000.0f), gain_q8_for(8.0f));
  for (int16_t v : samples) {
    EXPECT_EQ(0, v);
  }
}

TEST(AudioPipeline, ByteswapIsActuallyApplied) {
  // First sample with no DC blocker history: y[0] = x[0]. Unity gain,
  // high-cut off → defaults fast path. Output should be the byteswapped
  // input — a real check that we're not accidentally skipping the byteswap.
  std::vector<int16_t> samples{0x0102};
  DcBlockerState s{};
  HighCutState hc{};
  process_l16_payload_inplace(samples.data(), samples.size(), s, DC_BLOCKER_DEFAULT_R_Q15, hc, HIGH_CUT_A_Q15_OFF,
                              GAIN_Q8_UNITY);
  EXPECT_EQ(static_cast<int16_t>(0x0201), samples[0]);
}

// Helper: max |sample| of a buffer of *byteswapped* (post-DSP, big-endian)
// int16 values, computed by un-byteswapping and taking |x| in int32 so
// INT16_MIN's magnitude doesn't overflow.
uint16_t output_peak_abs(const std::vector<int16_t> &be) {
  uint16_t peak = 0;
  for (int16_t v : be) {
    const int16_t native = static_cast<int16_t>(byteswap_u16(static_cast<uint16_t>(v)));
    const uint16_t a = static_cast<uint16_t>(native < 0 ? -static_cast<int32_t>(native) : native);
    if (a > peak)
      peak = a;
  }
  return peak;
}

TEST(AudioPipeline, PeakReturnMatchesOutputMaxFastPath) {
  // Fast path (high-cut off + unity gain). The returned peak must equal
  // max |sample| of the actual output (post-DSP) so the meter reports
  // exactly what was streamed. Anything else is a bug.
  std::vector<int16_t> samples{0, 1000, -1000, 4242, -4242, 30000, -30000, 12345};
  DcBlockerState s{};
  HighCutState hc{};
  const uint16_t peak = process_l16_payload_inplace(samples.data(), samples.size(), s, DC_BLOCKER_DEFAULT_R_Q15, hc,
                                                    HIGH_CUT_A_Q15_OFF, GAIN_Q8_UNITY);
  EXPECT_EQ(output_peak_abs(samples), peak);
}

TEST(AudioPipeline, PeakReturnMatchesOutputMaxFullPath) {
  // Same invariant on the full path (taken because gain != unity).
  // Without this we'd have no guarantee the two branches of the loop
  // report identical peaks for the same audio.
  std::vector<int16_t> samples{0, 1000, -1000, 4242, -4242, 12345, -12345};
  DcBlockerState s{};
  HighCutState hc{};
  const uint16_t peak = process_l16_payload_inplace(samples.data(), samples.size(), s, DC_BLOCKER_DEFAULT_R_Q15, hc,
                                                    HIGH_CUT_A_Q15_OFF, gain_q8_for(4.0f));
  EXPECT_EQ(output_peak_abs(samples), peak);
}

TEST(AudioPipeline, PeakIsZeroOnSilence) {
  // Silence in → peak 0, on both paths. The dBFS conversion in
  // rtsp_audio.cpp relies on this exact 0 sentinel to publish the
  // silence floor rather than computing log10(0).
  std::vector<int16_t> samples(64, 0);
  DcBlockerState s_fast{};
  HighCutState hc_fast{};
  EXPECT_EQ(uint16_t{0}, process_l16_payload_inplace(samples.data(), samples.size(), s_fast, DC_BLOCKER_DEFAULT_R_Q15,
                                                     hc_fast, HIGH_CUT_A_Q15_OFF, GAIN_Q8_UNITY));

  std::vector<int16_t> samples_full(64, 0);
  DcBlockerState s_full{};
  HighCutState hc_full{};
  EXPECT_EQ(uint16_t{0},
            process_l16_payload_inplace(samples_full.data(), samples_full.size(), s_full, DC_BLOCKER_DEFAULT_R_Q15,
                                        hc_full, high_cut_a_q15_for(2000.0f, 32000.0f), gain_q8_for(8.0f)));
}

TEST(AudioPipeline, PeakHandlesInt16MinWithoutOverflow) {
  // |INT16_MIN| is 32768, which doesn't fit in int16 — a naive `-s`
  // would overflow and a naive `static_cast<uint16_t>(-s)` would
  // produce 0x8000 only by luck of the wrap. We use a widened negation
  // (int32) in `abs_i16`; this test pins that behavior down. Loud
  // input + max gain reliably saturates at least one sample to
  // INT16_MIN, which is the case we want to exercise.
  std::vector<int16_t> samples{-20000, 20000, -20000, 20000};
  DcBlockerState s{};
  HighCutState hc{};
  const uint16_t peak = process_l16_payload_inplace(samples.data(), samples.size(), s, DC_BLOCKER_DEFAULT_R_Q15, hc,
                                                    HIGH_CUT_A_Q15_OFF, gain_q8_for(GAIN_MAX));
  EXPECT_EQ(uint16_t{32768}, peak);
}

TEST(AudioPipeline, PeakReturnIsPerCallNotCumulative) {
  // Each call must report its own buffer's peak, not a running max
  // across calls. The caller (rtsp_audio.cpp) owns the 5 s rolling
  // window and resets `window_peak_abs_` to 0 on stop_streaming_ (so
  // RTSP PAUSE drops the HA meter to the silence floor). That reset
  // is only safe if the pipeline itself is stateless w.r.t. the peak
  // — if someone ever promoted the per-loop accumulator to a static
  // or instance variable, this test would catch it before the PAUSE
  // fix silently regressed.
  std::vector<int16_t> loud{20000, -20000, 20000, -20000};
  std::vector<int16_t> quiet{100, -100, 50, -50};
  DcBlockerState s_loud{};
  DcBlockerState s_quiet{};
  HighCutState hc_loud{};
  HighCutState hc_quiet{};
  const uint16_t loud_peak = process_l16_payload_inplace(loud.data(), loud.size(), s_loud, DC_BLOCKER_DEFAULT_R_Q15,
                                                         hc_loud, HIGH_CUT_A_Q15_OFF, GAIN_Q8_UNITY);
  const uint16_t quiet_peak = process_l16_payload_inplace(quiet.data(), quiet.size(), s_quiet, DC_BLOCKER_DEFAULT_R_Q15,
                                                          hc_quiet, HIGH_CUT_A_Q15_OFF, GAIN_Q8_UNITY);
  // If state leaked from the first call, quiet_peak would inherit the
  // ~20000 magnitude of `loud`. Hard-coded thresholds rather than a
  // ratio so the assertion stays readable.
  EXPECT_GT(loud_peak, uint16_t{15000});
  EXPECT_LT(quiet_peak, uint16_t{500});
}

TEST(AudioPipeline, PeakReflectsPostGain) {
  // The whole point of the peak meter is to surface clipping the gain
  // stage introduces, so the tap must be *after* gain. Quiet input
  // run through the pipeline at unity vs. 4× should produce a ~4× peak
  // — anything close to 1× would mean the tap is sitting upstream of
  // the gain multiply.
  const std::vector<int16_t> input{0, 1000, -1000, 1500, -1500, 1000};
  std::vector<int16_t> unity = input;
  std::vector<int16_t> amped = input;
  DcBlockerState s_unity{}, s_amped{};
  HighCutState hc_unity{}, hc_amped{};
  const uint16_t unity_peak = process_l16_payload_inplace(unity.data(), unity.size(), s_unity, DC_BLOCKER_DEFAULT_R_Q15,
                                                          hc_unity, HIGH_CUT_A_Q15_OFF, GAIN_Q8_UNITY);
  const uint16_t amped_peak = process_l16_payload_inplace(amped.data(), amped.size(), s_amped, DC_BLOCKER_DEFAULT_R_Q15,
                                                          hc_amped, HIGH_CUT_A_Q15_OFF, gain_q8_for(4.0f));
  // Allow some slack for the DC-blocker transient on the first sample.
  EXPECT_GT(amped_peak, unity_peak * 3);
  EXPECT_LT(amped_peak, unity_peak * 5);
}

TEST(AudioPipeline, HighCutOnAttenuatesHighFreq) {
  // A high-frequency input (alternating ±A) should be attenuated when
  // the high-cut stage is engaged. Compare peak magnitude of the
  // pipeline output (after un-byteswapping) with the high-cut off vs.
  // on; on must be strictly smaller.
  constexpr size_t kCount = 512;
  constexpr int16_t kAmp = 8000;
  std::vector<int16_t> off_samples(kCount);
  std::vector<int16_t> on_samples(kCount);
  for (size_t i = 0; i < kCount; i++) {
    off_samples[i] = on_samples[i] = (i & 1) ? -kAmp : kAmp;
  }

  DcBlockerState s_off{};
  DcBlockerState s_on{};
  HighCutState hc_off{};
  HighCutState hc_on{};
  process_l16_payload_inplace(off_samples.data(), kCount, s_off, DC_BLOCKER_DEFAULT_R_Q15, hc_off, HIGH_CUT_A_Q15_OFF,
                              GAIN_Q8_UNITY);
  process_l16_payload_inplace(on_samples.data(), kCount, s_on, DC_BLOCKER_DEFAULT_R_Q15, hc_on,
                              high_cut_a_q15_for(2000.0f, 32000.0f), GAIN_Q8_UNITY);

  auto peak_native = [](const std::vector<int16_t> &be) {
    int32_t peak = 0;
    // Skip the first 100 samples so the high-cut transient has settled.
    for (size_t i = 100; i < be.size(); i++) {
      const int16_t native = static_cast<int16_t>(byteswap_u16(static_cast<uint16_t>(be[i])));
      peak = std::max<int32_t>(peak, std::abs(static_cast<int32_t>(native)));
    }
    return peak;
  };

  EXPECT_LT(peak_native(on_samples), peak_native(off_samples));
}

}  // namespace
}  // namespace esphome::rtsp_audio::internal
