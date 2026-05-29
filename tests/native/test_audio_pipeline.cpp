#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "audio_pipeline.h"
#include "biquad.h"
#include "dc_blocker.h"
#include "gain.h"
#include "high_cut_biquad.h"
#include "low_cut_biquad.h"
#include "soft_limiter.h"

namespace esphome::rtsp_audio::internal {
namespace {

// Default low-cut Butterworth coefficients at 100 Hz / 32 kHz — what the
// component instantiates when no HA slider has been set. Computing it
// once per test keeps the call sites readable.
BiquadCoeffs default_lowcut() {
  return low_cut_butterworth_coeffs(static_cast<float>(LOW_CUT_DEFAULT_CUTOFF_HZ), 32000.0f);
}

// Reference implementation of the default (no-touch HA install) chain:
// DC blocker → low-cut biquad → byteswap. High-cut and gain are
// bypassed by default, so they don't appear here. Used to confirm
// the pipeline's full path matches a hand-rolled version stage for
// stage.
void reference_default_pipeline(std::vector<int16_t> &samples, DcBlockerState &dc, BiquadState &lc,
                                const BiquadCoeffs &lc_coeffs) {
  for (auto &sample : samples) {
    const int16_t after_dc = dc_blocker_step(sample, dc);
    const int16_t filtered = biquad_step(after_dc, lc, lc_coeffs);
    sample = static_cast<int16_t>(byteswap_u16(static_cast<uint16_t>(filtered)));
  }
}

TEST(AudioPipeline, DefaultsMatchReference) {
  // Same input through two independent state machines: the pipeline's
  // own loop vs a hand-rolled DC-blocker → low-cut → byteswap. Bytes
  // must match exactly. If they don't, a stage was reordered or one
  // of the bypass branches dropped something it shouldn't have.
  std::vector<int16_t> input{0, 1000, -1000, 4242, -4242, 30000, -30000, 12345};
  std::vector<int16_t> ref = input;
  std::vector<int16_t> got = input;
  DcBlockerState dc_ref{};
  DcBlockerState dc_got{};
  BiquadState s_ref{};
  BiquadState s_got{};
  BiquadState hc_got{};
  const BiquadCoeffs lc = default_lowcut();
  BiquadCoeffs hc{};
  SoftLimiterState sl{};

  reference_default_pipeline(ref, dc_ref, s_ref, lc);
  process_l16_payload_inplace(got.data(), got.size(), dc_got, s_got, lc,
                              /*lowcut_bypass=*/false, hc_got, hc, /*highcut_bypass=*/true, GAIN_Q8_UNITY, sl,
                              /*sl_bypass=*/true, 0.0f, 0.0f, 0.0f);

  EXPECT_EQ(ref, got);
}

TEST(AudioPipeline, ScaledPathSaturatesAtMaxGain) {
  // Loud input + max gain → output must clamp to INT16_MIN/MAX (in big
  // endian) and never wrap.
  std::vector<int16_t> samples{20000, -20000, 20000, -20000};
  DcBlockerState dc{};
  BiquadState s{};
  BiquadState hc{};
  const BiquadCoeffs lc = default_lowcut();
  BiquadCoeffs hc_coeffs{};
  SoftLimiterState sl{};
  process_l16_payload_inplace(samples.data(), samples.size(), dc, s, lc,
                              /*lowcut_bypass=*/false, hc, hc_coeffs, /*highcut_bypass=*/true, gain_q8_for(GAIN_MAX),
                              sl, /*sl_bypass=*/true, 0.0f, 0.0f, 0.0f);

  for (int16_t v : samples) {
    const int16_t native = static_cast<int16_t>(byteswap_u16(static_cast<uint16_t>(v)));
    EXPECT_TRUE(native == INT16_MAX || native == INT16_MIN) << "got " << native;
  }
}

TEST(AudioPipeline, ZeroInputZeroOutput) {
  // Silence in → silence out, regardless of gain or whether the cut
  // filters are on.
  std::vector<int16_t> samples(64, 0);
  DcBlockerState dc{};
  BiquadState s{};
  BiquadState hc{};
  const BiquadCoeffs lc = default_lowcut();
  const BiquadCoeffs hc_coeffs = high_cut_butterworth_coeffs(2000.0f, 32000.0f);
  SoftLimiterState sl{};
  process_l16_payload_inplace(samples.data(), samples.size(), dc, s, lc,
                              /*lowcut_bypass=*/false, hc, hc_coeffs, /*highcut_bypass=*/false, gain_q8_for(8.0f), sl,
                              /*sl_bypass=*/true, 0.0f, 0.0f, 0.0f);
  for (int16_t v : samples) {
    EXPECT_EQ(0, v);
  }
}

TEST(AudioPipeline, ByteswapIsActuallyApplied) {
  // Compare what the pipeline emits against a manual DC-blocker →
  // biquad → byteswap on the same input. Equality means the byteswap
  // is happening — a regression that silently dropped it would
  // produce native-endian output and this test would catch it.
  std::vector<int16_t> samples{0x4321};
  DcBlockerState dc_pipeline{};
  BiquadState pipeline_state{};
  BiquadState hc{};
  const BiquadCoeffs lc = default_lowcut();
  BiquadCoeffs hc_coeffs{};
  SoftLimiterState sl{};
  process_l16_payload_inplace(samples.data(), samples.size(), dc_pipeline, pipeline_state, lc, /*lowcut_bypass=*/false,
                              hc, hc_coeffs, /*highcut_bypass=*/true, GAIN_Q8_UNITY, sl, /*sl_bypass=*/true, 0.0f,
                              0.0f, 0.0f);

  DcBlockerState dc_manual{};
  BiquadState manual_state{};
  const int16_t after_dc = dc_blocker_step(static_cast<int16_t>(0x4321), dc_manual);
  const int16_t filtered = biquad_step(after_dc, manual_state, lc);
  const int16_t expected = static_cast<int16_t>(byteswap_u16(static_cast<uint16_t>(filtered)));
  EXPECT_EQ(expected, samples[0]);
  // Make sure DSP actually happened (input had non-symmetric bytes —
  // a no-op pipeline plus byteswap would produce 0x2143; we want to
  // catch the case where the DSP got bypassed entirely).
  EXPECT_NE(static_cast<int16_t>(0x4321), filtered);
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

TEST(AudioPipeline, PeakReturnMatchesOutput) {
  // The returned peak must equal max |sample| of the actual output
  // (post-DSP) so the meter reports exactly what was streamed.
  std::vector<int16_t> samples{0, 1000, -1000, 4242, -4242, 30000, -30000, 12345};
  DcBlockerState dc{};
  BiquadState s{};
  BiquadState hc{};
  const BiquadCoeffs lc = default_lowcut();
  BiquadCoeffs hc_coeffs{};
  SoftLimiterState sl{};
  const uint16_t peak =
      process_l16_payload_inplace(samples.data(), samples.size(), dc, s, lc,
                                  /*lowcut_bypass=*/false, hc, hc_coeffs, /*highcut_bypass=*/true, gain_q8_for(4.0f),
                                  sl, /*sl_bypass=*/true, 0.0f, 0.0f, 0.0f);
  EXPECT_EQ(output_peak_abs(samples), peak);
}

TEST(AudioPipeline, PeakIsZeroOnSilence) {
  // Silence in → peak 0. The dBFS conversion in rtsp_audio.cpp relies
  // on this exact 0 sentinel to publish the silence floor rather
  // than computing log10(0).
  std::vector<int16_t> samples(64, 0);
  DcBlockerState dc{};
  BiquadState s{};
  BiquadState hc{};
  const BiquadCoeffs lc = default_lowcut();
  const BiquadCoeffs hc_on = high_cut_butterworth_coeffs(2000.0f, 32000.0f);
  SoftLimiterState sl{};
  EXPECT_EQ(uint16_t{0}, process_l16_payload_inplace(samples.data(), samples.size(), dc, s, lc,
                                                     /*lowcut_bypass=*/false, hc, hc_on, /*highcut_bypass=*/false,
                                                     gain_q8_for(8.0f), sl, /*sl_bypass=*/true, 0.0f, 0.0f, 0.0f));
}

TEST(AudioPipeline, PeakHandlesInt16MinWithoutOverflow) {
  // |INT16_MIN| is 32768, which doesn't fit in int16 — a naive `-s`
  // would overflow. abs_i16 widens through int32; this pins the
  // behavior down.
  std::vector<int16_t> samples{-20000, 20000, -20000, 20000};
  DcBlockerState dc{};
  BiquadState s{};
  BiquadState hc{};
  const BiquadCoeffs lc = default_lowcut();
  BiquadCoeffs hc_coeffs{};
  SoftLimiterState sl{};
  const uint16_t peak = process_l16_payload_inplace(samples.data(), samples.size(), dc, s, lc,
                                                    /*lowcut_bypass=*/false, hc, hc_coeffs, /*highcut_bypass=*/true,
                                                    gain_q8_for(GAIN_MAX), sl, /*sl_bypass=*/true, 0.0f, 0.0f, 0.0f);
  EXPECT_EQ(uint16_t{32768}, peak);
}

TEST(AudioPipeline, PeakReflectsPostGain) {
  // The peak meter sits *after* the gain stage so it surfaces clipping
  // the gain introduces. Quiet input run at unity vs 4× should give
  // a ~4× peak — anything close to 1× means the tap is sitting
  // upstream of the gain multiply.
  const std::vector<int16_t> input{0, 1000, -1000, 1500, -1500, 1000};
  std::vector<int16_t> unity = input;
  std::vector<int16_t> amped = input;
  DcBlockerState dc_unity{}, dc_amped{};
  BiquadState s_unity{}, s_amped{};
  BiquadState hc_unity{}, hc_amped{};
  const BiquadCoeffs lc = default_lowcut();
  BiquadCoeffs hc_coeffs{};
  SoftLimiterState sl_unity{}, sl_amped{};
  const uint16_t unity_peak =
      process_l16_payload_inplace(unity.data(), unity.size(), dc_unity, s_unity, lc,
                                  /*lowcut_bypass=*/false, hc_unity, hc_coeffs, /*highcut_bypass=*/true, GAIN_Q8_UNITY,
                                  sl_unity, /*sl_bypass=*/true, 0.0f, 0.0f, 0.0f);
  const uint16_t amped_peak = process_l16_payload_inplace(amped.data(), amped.size(), dc_amped, s_amped, lc,
                                                          /*lowcut_bypass=*/false, hc_amped, hc_coeffs,
                                                          /*highcut_bypass=*/true, gain_q8_for(4.0f), sl_amped,
                                                          /*sl_bypass=*/true, 0.0f, 0.0f, 0.0f);
  // Slack for the low-cut transient on the first sample.
  EXPECT_GT(amped_peak, unity_peak * 3);
  EXPECT_LT(amped_peak, unity_peak * 5);
}

TEST(AudioPipeline, HighCutOnAttenuatesHighFreq) {
  // High-frequency input (alternating ±A) gets attenuated when the
  // high-cut stage is engaged. Compare peak magnitude with the
  // high-cut off vs on; on must be strictly smaller.
  constexpr size_t kCount = 512;
  constexpr int16_t kAmp = 8000;
  std::vector<int16_t> off_samples(kCount);
  std::vector<int16_t> on_samples(kCount);
  for (size_t i = 0; i < kCount; i++) {
    off_samples[i] = on_samples[i] = (i & 1) ? -kAmp : kAmp;
  }

  DcBlockerState dc_off{}, dc_on{};
  BiquadState s_off{}, s_on{};
  BiquadState hc_off{}, hc_on{};
  const BiquadCoeffs lc = default_lowcut();
  BiquadCoeffs hc_off_coeffs{};
  const BiquadCoeffs hc_on_coeffs = high_cut_butterworth_coeffs(2000.0f, 32000.0f);
  SoftLimiterState sl_off{}, sl_on{};
  process_l16_payload_inplace(off_samples.data(), kCount, dc_off, s_off, lc,
                              /*lowcut_bypass=*/false, hc_off, hc_off_coeffs, /*highcut_bypass=*/true, GAIN_Q8_UNITY,
                              sl_off, /*sl_bypass=*/true, 0.0f, 0.0f, 0.0f);
  process_l16_payload_inplace(on_samples.data(), kCount, dc_on, s_on, lc,
                              /*lowcut_bypass=*/false, hc_on, hc_on_coeffs, /*highcut_bypass=*/false, GAIN_Q8_UNITY,
                              sl_on, /*sl_bypass=*/true, 0.0f, 0.0f, 0.0f);

  auto peak_native = [](const std::vector<int16_t> &be) {
    int32_t peak = 0;
    for (size_t i = 100; i < be.size(); i++) {
      const int16_t native = static_cast<int16_t>(byteswap_u16(static_cast<uint16_t>(be[i])));
      peak = std::max<int32_t>(peak, std::abs(static_cast<int32_t>(native)));
    }
    return peak;
  };

  EXPECT_LT(peak_native(on_samples), peak_native(off_samples));
}

TEST(AudioPipeline, LowCutBypassStillRemovesDC) {
  // The whole point of the dedicated DC blocker: even with the
  // user-facing low-cut turned off, MEMS DC bias still gets killed.
  // Feed a constant DC offset with low-cut bypassed and confirm the
  // output settles near zero — anyone setting `low_cut_frequency_hz`
  // to 0 in HA shouldn't end up streaming a biased signal.
  constexpr size_t kCount = 50000;  // ~1.5 s at 32 kHz; many time constants
  constexpr int16_t kDc = 1000;
  std::vector<int16_t> samples(kCount, kDc);
  DcBlockerState dc{};
  BiquadState s{};
  BiquadState hc{};
  BiquadCoeffs lc_coeffs{};  // unused when bypassed
  BiquadCoeffs hc_coeffs{};
  SoftLimiterState sl{};
  process_l16_payload_inplace(samples.data(), samples.size(), dc, s, lc_coeffs,
                              /*lowcut_bypass=*/true, hc, hc_coeffs, /*highcut_bypass=*/true, GAIN_Q8_UNITY, sl,
                              /*sl_bypass=*/true, 0.0f, 0.0f, 0.0f);

  // Last sample (post-byteswap) should be very close to zero.
  const int16_t last = static_cast<int16_t>(byteswap_u16(static_cast<uint16_t>(samples.back())));
  EXPECT_LE(std::abs(static_cast<int>(last)), 10);
}

}  // namespace
}  // namespace esphome::rtsp_audio::internal
