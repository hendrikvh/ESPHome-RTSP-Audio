#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "audio_pipeline.h"
#include "gain.h"
#include "soft_limiter.h"

namespace esphome::rtsp_audio::internal {
namespace {

// Threshold used for most tests: -3 dBFS = ~0.708 linear = ~23,170 amplitude.
const float kThreshold = limiter_db_to_linear(-3.0f);
// Fast attack (5 ms) and slow release (100 ms) at 32 kHz.
const float kAttack = limiter_time_coeff(5.0f, 32000.0f);
const float kRelease = limiter_time_coeff(100.0f, 32000.0f);

// Run N samples of amplitude `amp` through the limiter and return the last
// output value. Used to drive the envelope to a steady state.
int16_t drive_limiter(SoftLimiterState &state, int16_t amp, size_t n) {
  int16_t out = 0;
  for (size_t i = 0; i < n; i++) {
    const int16_t s = (i & 1) ? -amp : amp;
    out = soft_limiter_step(s, state, kThreshold, kAttack, kRelease);
  }
  return out;
}

TEST(SoftLimiter, SilencePassesThrough) {
  // Zero input → zero output regardless of state; env stays at 0.
  SoftLimiterState state{};
  for (int i = 0; i < 64; i++) {
    EXPECT_EQ(0, soft_limiter_step(0, state, kThreshold, kAttack, kRelease));
  }
  EXPECT_FLOAT_EQ(0.0f, state.env);
}

TEST(SoftLimiter, SignalBelowThresholdIsUnchanged) {
  // A small sine-like signal well below the threshold must emerge unchanged.
  // We test a single quiet sample that, after normalisation, is clearly below
  // the threshold linear value (~0.708). Use amplitude 1000 (~0.031 linear).
  SoftLimiterState state{};
  const int16_t in = 1000;
  const int16_t out = soft_limiter_step(in, state, kThreshold, kAttack, kRelease);
  EXPECT_EQ(in, out);
}

TEST(SoftLimiter, GainReductionEngagesAboveThreshold) {
  // Drive the limiter with a signal well above the threshold until the
  // envelope converges. The steady-state output amplitude must be at or
  // below the threshold ceiling (~23,170 counts).
  SoftLimiterState state{};
  constexpr int16_t kAmp = 30000;   // ~0.916 linear, well above -3 dBFS
  constexpr size_t kSettle = 4000;  // ~125 ms at 32 kHz; many attack time constants
  drive_limiter(state, kAmp, kSettle);

  // Check a few more output samples after settling.
  for (int i = 0; i < 20; i++) {
    const int16_t s = (i & 1) ? -kAmp : kAmp;
    const int16_t out = soft_limiter_step(s, state, kThreshold, kAttack, kRelease);
    const int32_t abs_out = out < 0 ? -static_cast<int32_t>(out) : out;
    // Allow a small margin above the nominal threshold due to attack lag.
    EXPECT_LE(abs_out, static_cast<int32_t>(kThreshold * 32768.0f) + 200)
        << "Output " << abs_out << " exceeded threshold after settling";
  }
}

TEST(SoftLimiter, GainIsProportional) {
  // When the envelope is exactly at 2× threshold, the gain reduction
  // should halve the signal to approximately threshold. We can't test
  // the internal state directly, but we can drive to a known level and
  // verify the output is roughly proportional.
  SoftLimiterState state{};
  // Drive to steady state at 2× threshold (~0.5 linear = ~16,384 counts after limiting).
  const float two_threshold = kThreshold * 2.0f;
  const int16_t amp = static_cast<int16_t>(two_threshold * 32768.0f);
  drive_limiter(state, amp, 5000);

  // After settling, output should be near threshold (not full amplitude).
  const int16_t out = soft_limiter_step(amp, state, kThreshold, kAttack, kRelease);
  const int32_t abs_out = out < 0 ? -static_cast<int32_t>(out) : out;
  const int32_t threshold_counts = static_cast<int32_t>(kThreshold * 32768.0f);
  // Output must be at most 1.1× threshold (10 % tolerance for floating-point rounding).
  EXPECT_LE(abs_out, static_cast<int32_t>(threshold_counts * 1.1f));
  // Output must be at least 0.8× threshold (limiter isn't over-attenuating).
  EXPECT_GE(abs_out, static_cast<int32_t>(threshold_counts * 0.8f));
}

TEST(SoftLimiter, AttackRisesQuickly) {
  // After a sudden onset transient, the envelope must have risen
  // substantially within one attack time constant (5 ms = 160 samples).
  SoftLimiterState state{};
  const int16_t kAmp = 30000;
  // Feed one attack time constant's worth of loud samples.
  for (int i = 0; i < 160; i++) {
    soft_limiter_step((i & 1) ? -kAmp : kAmp, state, kThreshold, kAttack, kRelease);
  }
  // Envelope should have risen significantly (at least 50 % of amplitude).
  EXPECT_GT(state.env, 0.5f * (kAmp / 32768.0f));
}

TEST(SoftLimiter, ReleasedSlowlyAfterTransient) {
  // After driving loud input and then going silent, the envelope must
  // still be above a meaningful level after one attack-time interval —
  // i.e. release is much slower than attack.
  SoftLimiterState state{};
  const int16_t kAmp = 30000;
  drive_limiter(state, kAmp, 3000);  // saturate

  const float env_before = state.env;
  // Feed silence for one attack time constant (160 samples).
  for (int i = 0; i < 160; i++) {
    soft_limiter_step(0, state, kThreshold, kAttack, kRelease);
  }
  // Envelope should still be close to env_before (slow release).
  EXPECT_GT(state.env, env_before * 0.8f);
}

TEST(SoftLimiter, StateResetClearsEnvelope) {
  // A fresh SoftLimiterState{} after a "session" must start with env=0.
  SoftLimiterState state{};
  drive_limiter(state, 30000, 1000);
  EXPECT_GT(state.env, 0.0f);  // confirm it was driven

  state = {};  // reset, as done in start_streaming_()
  EXPECT_FLOAT_EQ(0.0f, state.env);

  // First quiet sample after reset must not be attenuated.
  const int16_t out = soft_limiter_step(1000, state, kThreshold, kAttack, kRelease);
  EXPECT_EQ(1000, out);
}

TEST(SoftLimiter, NeverOverflowsInt16) {
  // For any int16 input, the output must stay within the int16 range.
  // Test corner cases: INT16_MAX, INT16_MIN, and alternating ±32767.
  SoftLimiterState state{};
  const std::vector<int16_t> edges{INT16_MAX, INT16_MIN, INT16_MAX, INT16_MIN, 0, INT16_MAX};
  for (int16_t s : edges) {
    const int16_t out = soft_limiter_step(s, state, kThreshold, kAttack, kRelease);
    EXPECT_GE(static_cast<int>(out), INT16_MIN);
    EXPECT_LE(static_cast<int>(out), INT16_MAX);
  }
}

TEST(SoftLimiter, FullScaleThresholdIsPassthrough) {
  // Threshold at 0 dBFS (linear = 1.0) means the limiter never fires
  // for any valid int16 input — every sample is below or at full scale.
  const float threshold_full = limiter_db_to_linear(0.0f);  // 1.0
  const float attack = limiter_time_coeff(5.0f, 32000.0f);
  const float release = limiter_time_coeff(100.0f, 32000.0f);
  for (int16_t amp : {int16_t{1000}, int16_t{10000}, int16_t{20000}, int16_t{30000}}) {
    SoftLimiterState s{};
    const int16_t out = soft_limiter_step(amp, s, threshold_full, attack, release);
    EXPECT_EQ(amp, out);
  }
}

TEST(SoftLimiter, PipelineWithLimiterReducesClipping) {
  // With high gain and loud input, running the pipeline with the limiter
  // ENABLED should produce a strictly lower peak than with it BYPASSED
  // in the settled region (after the envelope converges). Both paths hit
  // the hard INT16 clip on the initial transient; the difference shows
  // in the tail once the limiter's envelope follower has caught up.
  constexpr size_t kCount = 6400;  // 200 ms at 32 kHz; well past attack settling
  constexpr int16_t kAmp = 8000;

  // Build two identical alternating-sign input buffers.
  std::vector<int16_t> with_limiter(kCount), without_limiter(kCount);
  for (size_t i = 0; i < kCount; i++) {
    with_limiter[i] = without_limiter[i] = (i & 1) ? -kAmp : kAmp;
  }

  // Dummy filter state / coefficients (lowcut + highcut both bypassed).
  DcBlockerState dc_on{}, dc_off{};
  BiquadState lc_on{}, lc_off{};
  BiquadState hc_on{}, hc_off{};
  BiquadCoeffs lc_coeffs{}, hc_coeffs{};

  SoftLimiterState sl_on{};
  SoftLimiterState sl_off{};

  const float threshold = limiter_db_to_linear(-3.0f);
  const float attack = limiter_time_coeff(5.0f, 32000.0f);
  const float release = limiter_time_coeff(100.0f, 32000.0f);
  // High gain: +20 dB = 10× linear; without limiter this clips hard.
  const int32_t high_gain = gain_q8_for(10.0f);

  process_l16_payload_inplace(with_limiter.data(), kCount, dc_on, lc_on, lc_coeffs, /*lowcut_bypass=*/true, hc_on,
                              hc_coeffs, /*highcut_bypass=*/true, high_gain, sl_on, /*sl_bypass=*/false, threshold,
                              attack, release);

  process_l16_payload_inplace(without_limiter.data(), kCount, dc_off, lc_off, lc_coeffs, /*lowcut_bypass=*/true, hc_off,
                              hc_coeffs, /*highcut_bypass=*/true, high_gain, sl_off, /*sl_bypass=*/true, 0.0f, 0.0f,
                              0.0f);

  // Measure peak over the TAIL of the buffer (last quarter) after settling.
  // The first quarter may contain transient clips before the envelope converges.
  auto tail_peak = [](const std::vector<int16_t> &buf) {
    int32_t p = 0;
    const size_t start = buf.size() * 3 / 4;
    for (size_t i = start; i < buf.size(); i++) {
      const int16_t native = static_cast<int16_t>(__builtin_bswap16(static_cast<uint16_t>(buf[i])));
      p = std::max(p, static_cast<int32_t>(native < 0 ? -static_cast<int32_t>(native) : native));
    }
    return p;
  };

  EXPECT_LT(tail_peak(with_limiter), tail_peak(without_limiter))
      << "Limiter should produce lower settled peak than hard clip at high gain";
}

// ── last_gain / packet_min_gain tests (gain-reduction sensor coverage) ───────

TEST(SoftLimiterGainReduction, LastGainIsUnityBelowThreshold) {
  // When the signal stays below the threshold, no reduction is applied and
  // last_gain must be exactly 1.0 (= 0 dB reduction reported to the sensor).
  SoftLimiterState state{};
  soft_limiter_step(1000, state, kThreshold, kAttack, kRelease);
  EXPECT_FLOAT_EQ(1.0f, state.last_gain);
}

TEST(SoftLimiterGainReduction, LastGainBelowUnityWhenLimiting) {
  // After the envelope exceeds the threshold, last_gain must be < 1.0.
  SoftLimiterState state{};
  drive_limiter(state, 30000, 3000);  // drive well above threshold
  const int16_t amp = 30000;
  soft_limiter_step(amp, state, kThreshold, kAttack, kRelease);
  EXPECT_LT(state.last_gain, 1.0f);
}

TEST(SoftLimiterGainReduction, LastGainIsProportionalToThresholdOverEnv) {
  // When the envelope is stable at amplitude `amp`, last_gain ≈ threshold / (amp/32768).
  SoftLimiterState state{};
  const int16_t amp = 30000;
  drive_limiter(state, amp, 5000);  // stabilise
  soft_limiter_step(amp, state, kThreshold, kAttack, kRelease);
  // Expected gain ≈ threshold / env; env ≈ amp/32768 after settling.
  const float expected_gain = kThreshold / state.env;
  EXPECT_NEAR(state.last_gain, expected_gain, 0.01f);
}

TEST(SoftLimiterGainReduction, LastGainResetsAfterStateReset) {
  // After a session reset (state = {}), the very first quiet sample must
  // report last_gain == 1.0 — a stale value from a prior session must not bleed in.
  SoftLimiterState state{};
  drive_limiter(state, 30000, 1000);
  EXPECT_LT(state.last_gain, 1.0f);  // confirm it was limiting

  state = {};
  soft_limiter_step(1000, state, kThreshold, kAttack, kRelease);
  EXPECT_FLOAT_EQ(1.0f, state.last_gain);
}

TEST(SoftLimiterGainReduction, PacketMinGainIsUnityWhenNoLimiting) {
  // A full packet of quiet samples must leave packet_min_gain at 1.0.
  constexpr size_t kCount = 640;  // 20 ms at 32 kHz
  std::vector<int16_t> samples(kCount, 1000);

  DcBlockerState dc{};
  BiquadState lc{}, hc{};
  BiquadCoeffs lc_coeffs{}, hc_coeffs{};
  SoftLimiterState sl{};

  process_l16_payload_inplace(samples.data(), kCount, dc, lc, lc_coeffs, /*lowcut_bypass=*/true, hc, hc_coeffs,
                              /*highcut_bypass=*/true, GAIN_Q8_UNITY, sl, /*sl_bypass=*/false, kThreshold, kAttack,
                              kRelease);

  EXPECT_FLOAT_EQ(1.0f, sl.packet_min_gain);
}

TEST(SoftLimiterGainReduction, PacketMinGainBelowUnityWhenLimiting) {
  // Loud samples above the threshold must drive packet_min_gain below 1.0.
  // First warm up the envelope so the limiter is already engaged, then run a packet.
  constexpr size_t kCount = 640;
  const int16_t kAmp = 30000;

  DcBlockerState dc{};
  BiquadState lc{}, hc{};
  BiquadCoeffs lc_coeffs{}, hc_coeffs{};
  SoftLimiterState sl{};

  // Warm-up: drive the envelope well above threshold.
  std::vector<int16_t> warm(3200);
  for (size_t i = 0; i < warm.size(); i++)
    warm[i] = (i & 1) ? -kAmp : kAmp;
  process_l16_payload_inplace(warm.data(), warm.size(), dc, lc, lc_coeffs, /*lowcut_bypass=*/true, hc, hc_coeffs,
                              /*highcut_bypass=*/true, GAIN_Q8_UNITY, sl, /*sl_bypass=*/false, kThreshold, kAttack,
                              kRelease);

  // Now measure a fresh packet with the envelope already above threshold.
  DcBlockerState dc2{};
  BiquadState lc2{}, hc2{};
  std::vector<int16_t> pkt(kCount);
  for (size_t i = 0; i < kCount; i++)
    pkt[i] = (i & 1) ? -kAmp : kAmp;
  process_l16_payload_inplace(pkt.data(), kCount, dc2, lc2, lc_coeffs, /*lowcut_bypass=*/true, hc2, hc_coeffs,
                              /*highcut_bypass=*/true, GAIN_Q8_UNITY, sl, /*sl_bypass=*/false, kThreshold, kAttack,
                              kRelease);

  EXPECT_LT(sl.packet_min_gain, 1.0f);
}

TEST(SoftLimiterGainReduction, PacketMinGainIsMinNotLast) {
  // packet_min_gain must be the MINIMUM gain applied in the packet, not the
  // gain of the last sample. Construct a scenario where gain varies: first
  // a loud section drives the envelope up (low gain), then a quiet section
  // lets it partially recover (higher gain). The minimum must match the
  // lower of the two, not the final value.
  constexpr size_t kHalf = 1600;  // 50 ms each half
  std::vector<int16_t> samples(kHalf * 2);
  for (size_t i = 0; i < kHalf; i++)
    samples[i] = (i & 1) ? -30000 : 30000;  // loud
  for (size_t i = kHalf; i < kHalf * 2; i++)
    samples[i] = 500;  // quiet

  DcBlockerState dc{};
  BiquadState lc{}, hc{};
  BiquadCoeffs lc_coeffs{}, hc_coeffs{};
  SoftLimiterState sl{};

  // Warm up the limiter first so it's already engaged at the start of the packet.
  std::vector<int16_t> warm(3200);
  for (size_t i = 0; i < warm.size(); i++)
    warm[i] = (i & 1) ? -30000 : 30000;
  {
    DcBlockerState dw{};
    BiquadState lw{}, hw{};
    process_l16_payload_inplace(warm.data(), warm.size(), dw, lw, lc_coeffs, /*lowcut_bypass=*/true, hw, hc_coeffs,
                                /*highcut_bypass=*/true, GAIN_Q8_UNITY, sl, /*sl_bypass=*/false, kThreshold, kAttack,
                                kRelease);
  }

  process_l16_payload_inplace(samples.data(), kHalf * 2, dc, lc, lc_coeffs, /*lowcut_bypass=*/true, hc, hc_coeffs,
                              /*highcut_bypass=*/true, GAIN_Q8_UNITY, sl, /*sl_bypass=*/false, kThreshold, kAttack,
                              kRelease);

  // The last_gain of the last (quiet) sample should be higher than packet_min_gain,
  // because the minimum occurred earlier during the loud half.
  EXPECT_LT(sl.packet_min_gain, sl.last_gain);
}

TEST(SoftLimiterGainReduction, PacketMinGainResetsEachPacket) {
  // Each call to process_l16_payload_inplace must reset packet_min_gain to
  // 1.0 before accumulating the current packet's minimum. Verify by seeding a
  // stale low value and confirming it is overwritten when the new packet has no
  // limiting (fresh envelope, quiet input → every sample gets gain = 1.0).
  constexpr size_t kCount = 640;

  DcBlockerState dc{};
  BiquadState lc{}, hc{};
  BiquadCoeffs lc_coeffs{}, hc_coeffs{};
  SoftLimiterState sl{};
  sl.packet_min_gain = 0.1f;  // stale value from a hypothetical prior loud packet

  // Fresh envelope (env=0) + quiet input → all samples below threshold → gain=1.0.
  std::vector<int16_t> quiet(kCount, 1000);
  process_l16_payload_inplace(quiet.data(), kCount, dc, lc, lc_coeffs, /*lowcut_bypass=*/true, hc, hc_coeffs,
                              /*highcut_bypass=*/true, GAIN_Q8_UNITY, sl, /*sl_bypass=*/false, kThreshold, kAttack,
                              kRelease);

  // packet_min_gain must reflect this packet's minimum (1.0), not the stale 0.1.
  EXPECT_FLOAT_EQ(1.0f, sl.packet_min_gain);
}

TEST(SoftLimiterGainReduction, PacketMinGainNotWrittenWhenBypassed) {
  // When sl_bypass=true the pipeline must not touch packet_min_gain.
  // Seed a known value and verify it is unchanged after a bypassed call.
  constexpr size_t kCount = 640;
  std::vector<int16_t> samples(kCount, 20000);

  DcBlockerState dc{};
  BiquadState lc{}, hc{};
  BiquadCoeffs lc_coeffs{}, hc_coeffs{};
  SoftLimiterState sl{};
  sl.packet_min_gain = 0.42f;  // arbitrary sentinel

  process_l16_payload_inplace(samples.data(), kCount, dc, lc, lc_coeffs, /*lowcut_bypass=*/true, hc, hc_coeffs,
                              /*highcut_bypass=*/true, GAIN_Q8_UNITY, sl, /*sl_bypass=*/true, kThreshold, kAttack,
                              kRelease);

  EXPECT_FLOAT_EQ(0.42f, sl.packet_min_gain);  // untouched
}

}  // namespace
}  // namespace esphome::rtsp_audio::internal
