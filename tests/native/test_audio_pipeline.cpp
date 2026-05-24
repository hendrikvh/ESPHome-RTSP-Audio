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
                              high_cut_a_q15_for(2000.0f, 16000.0f), gain_q8_for(8.0f));
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
                              high_cut_a_q15_for(2000.0f, 16000.0f), GAIN_Q8_UNITY);

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
