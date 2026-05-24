#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

#include "audio_pipeline.h"
#include "dc_blocker.h"
#include "gain.h"

namespace esphome::rtsp_audio::internal {
namespace {

// Reference implementation of what the pre-gain build did: DC-blocker
// then byteswap. Used to confirm the unity-gain fast path is
// bit-identical to the legacy loop.
void reference_unity_pipeline(std::vector<int16_t> &samples, DcBlockerState &s, int32_t r_q15) {
  for (auto &sample : samples) {
    const int16_t filtered = dc_blocker_step(sample, s, r_q15);
    sample = static_cast<int16_t>(byteswap_u16(static_cast<uint16_t>(filtered)));
  }
}

TEST(AudioPipeline, UnityMatchesReference) {
  // Same input, two state machines, two paths through the code. The
  // pipeline's fast-path must produce byte-for-byte the same payload as
  // the pre-gain implementation, because that's what existing RTSP
  // consumers have been receiving.
  std::vector<int16_t> input{0, 1000, -1000, 4242, -4242, 30000, -30000, 12345};
  std::vector<int16_t> ref = input;
  std::vector<int16_t> got = input;
  DcBlockerState s_ref{};
  DcBlockerState s_got{};

  reference_unity_pipeline(ref, s_ref, DC_BLOCKER_DEFAULT_R_Q15);
  process_l16_payload_inplace(got.data(), got.size(), s_got, DC_BLOCKER_DEFAULT_R_Q15, GAIN_Q8_UNITY);

  EXPECT_EQ(ref, got);
}

TEST(AudioPipeline, ScaledPathSaturatesAtMaxGain) {
  // Loud input + max gain → output must clamp to INT16_MIN/MAX (in big
  // endian) and never wrap. We check the int16 reinterpretation of the
  // byteswapped result.
  std::vector<int16_t> samples{20000, -20000, 20000, -20000};
  DcBlockerState s{};
  process_l16_payload_inplace(samples.data(), samples.size(), s, DC_BLOCKER_DEFAULT_R_Q15, gain_q8_for(GAIN_MAX));

  // Convert each big-endian word back to native and check the magnitude
  // hit the int16 rails (sign-agnostic — DC blocker output can flip
  // sign on the first transient).
  for (int16_t v : samples) {
    const int16_t native = static_cast<int16_t>(byteswap_u16(static_cast<uint16_t>(v)));
    EXPECT_TRUE(native == INT16_MAX || native == INT16_MIN) << "got " << native;
  }
}

TEST(AudioPipeline, ZeroInputZeroOutput) {
  // No matter the gain, silence in stays silence out. Even
  // byteswapped, zero is zero.
  std::vector<int16_t> samples(64, 0);
  DcBlockerState s{};
  process_l16_payload_inplace(samples.data(), samples.size(), s, DC_BLOCKER_DEFAULT_R_Q15, gain_q8_for(8.0f));
  for (int16_t v : samples) {
    EXPECT_EQ(0, v);
  }
}

TEST(AudioPipeline, ByteswapIsActuallyApplied) {
  // First sample with no DC blocker history: y[0] = x[0]. Unity gain.
  // So the output should be the byteswapped input — a real check that
  // we're not accidentally skipping the byteswap.
  std::vector<int16_t> samples{0x0102};
  DcBlockerState s{};
  process_l16_payload_inplace(samples.data(), samples.size(), s, DC_BLOCKER_DEFAULT_R_Q15, GAIN_Q8_UNITY);
  EXPECT_EQ(static_cast<int16_t>(0x0201), samples[0]);
}

}  // namespace
}  // namespace esphome::rtsp_audio::internal
