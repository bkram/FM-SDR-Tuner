#include "signal_level.h"

#include <algorithm>
#include <array>
#include <cmath>
#include "cpu_features.h"

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) ||             \
    defined(_M_IX86)
#include <immintrin.h>
#endif

#if defined(__aarch64__) || defined(__arm__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif

namespace {

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) ||             \
    defined(_M_IX86)
#if defined(__has_attribute)
#if __has_attribute(target)
#define SIGLEV_HAS_AVX2 1
#define SIGLEV_AVX2_TARGET __attribute__((target("avx2,fma")))
#endif
#elif defined(__GNUC__)
#define SIGLEV_HAS_AVX2 1
#define SIGLEV_AVX2_TARGET __attribute__((target("avx2,fma")))
#endif
#endif

#ifndef SIGLEV_HAS_AVX2
#define SIGLEV_HAS_AVX2 0
#define SIGLEV_AVX2_TARGET
#endif

double computeNormalizedIqPowerSumScalar(const uint8_t *iq, size_t count,
                                       const float *lut) {
  double powerSum = 0.0;
  size_t i = 0;
  for (; i + 4 <= count; i += 4) {
    powerSum += lut[iq[i]];
    powerSum += lut[iq[i + 1]];
    powerSum += lut[iq[i + 2]];
    powerSum += lut[iq[i + 3]];
  }
  for (; i < count; i++) {
    powerSum += lut[iq[i]];
  }
  return powerSum;
}

#if SIGLEV_HAS_AVX2
SIGLEV_AVX2_TARGET double
computeNormalizedIqPowerSumAvx2(const uint8_t *iq, size_t count,
                               const float *lut) {
  __m256 acc = _mm256_setzero_ps();
  size_t i = 0;
  for (; i + 8 <= count; i += 8) {
    const __m128i b8 =
        _mm_loadl_epi64(reinterpret_cast<const __m128i *>(iq + i));
    const __m256i idx = _mm256_cvtepu8_epi32(b8);
    const __m256 vals = _mm256_i32gather_ps(lut, idx, 4);
    acc = _mm256_add_ps(acc, vals);
  }
  alignas(32) float lanes[8];
  _mm256_store_ps(lanes, acc);
  double powerSum = lanes[0] + lanes[1] + lanes[2] + lanes[3] + lanes[4] +
                   lanes[5] + lanes[6] + lanes[7];
  for (; i < count; i++) {
    powerSum += lut[iq[i]];
  }
  return powerSum;
}
#endif

#if defined(__aarch64__) || defined(__arm__) || defined(_M_ARM64)
double computeNormalizedIqPowerSumNeon(const uint8_t *iq, size_t count) {
  const float32x4_t vMid = vdupq_n_f32(127.0f);
  const float32x4_t vInv = vdupq_n_f32(1.0f / 127.5f);
  float32x4_t acc = vdupq_n_f32(0.0f);
  size_t i = 0;
  for (; i + 16 <= count; i += 16) {
    const uint8x16_t v = vld1q_u8(iq + i);
    const uint16x8_t lo16 = vmovl_u8(vget_low_u8(v));
    const uint16x8_t hi16 = vmovl_u8(vget_high_u8(v));
    const uint32x4_t lo32a = vmovl_u16(vget_low_u16(lo16));
    const uint32x4_t lo32b = vmovl_u16(vget_high_u16(lo16));
    const uint32x4_t hi32a = vmovl_u16(vget_low_u16(hi16));
    const uint32x4_t hi32b = vmovl_u16(vget_high_u16(hi16));
    float32x4_t f0 = vcvtq_f32_u32(lo32a);
    float32x4_t f1 = vcvtq_f32_u32(lo32b);
    float32x4_t f2 = vcvtq_f32_u32(hi32a);
    float32x4_t f3 = vcvtq_f32_u32(hi32b);
    f0 = vmulq_f32(vsubq_f32(f0, vMid), vInv);
    f1 = vmulq_f32(vsubq_f32(f1, vMid), vInv);
    f2 = vmulq_f32(vsubq_f32(f2, vMid), vInv);
    f3 = vmulq_f32(vsubq_f32(f3, vMid), vInv);
    acc = vaddq_f32(acc, vmulq_f32(f0, f0));
    acc = vaddq_f32(acc, vmulq_f32(f1, f1));
    acc = vaddq_f32(acc, vmulq_f32(f2, f2));
    acc = vaddq_f32(acc, vmulq_f32(f3, f3));
  }
#if defined(__aarch64__)
  double powerSum = static_cast<double>(vaddvq_f32(acc));
#else
  float tmp[4];
  vst1q_f32(tmp, acc);
  double powerSum = tmp[0] + tmp[1] + tmp[2] + tmp[3];
#endif
  for (; i < count; i++) {
    const float norm = (static_cast<float>(iq[i]) - 127.0f) * (1.0f / 127.5f);
    powerSum += static_cast<double>(norm * norm);
  }
  return powerSum;
}
#endif

double computeNormalizedIqPowerSum(const uint8_t *iq, size_t samples) {
  static const std::array<float, 256> kNormSqLut = []() {
    std::array<float, 256> lut{};
    for (int v = 0; v < 256; v++) {
      const float norm = (static_cast<float>(v) - 127.0f) / 127.5f;
      lut[static_cast<size_t>(v)] = norm * norm;
    }
    return lut;
  }();
  const size_t count = samples * 2;
  static const CPUFeatures cpu = detectCPUFeatures();
#if SIGLEV_HAS_AVX2
  if (cpu.avx2 && cpu.fma) {
    return computeNormalizedIqPowerSumAvx2(iq, count, kNormSqLut.data());
  }
#endif
#if defined(__aarch64__) || defined(__arm__) || defined(_M_ARM64)
  if (cpu.neon) {
    return computeNormalizedIqPowerSumNeon(iq, count);
  }
#endif
  return computeNormalizedIqPowerSumScalar(iq, count, kNormSqLut.data());
}

} // namespace

SignalLevelResult computeSignalLevel(const uint8_t *iq, size_t samples,
                                    int appliedGainDb, double gainCompFactor,
                                    double signalBiasDb, double floorDbfs,
                                    double ceilDbfs) {
  SignalLevelResult out{};
  if (!iq || samples == 0) {
    return out;
  }

  double sumI = 0.0;
  double sumQ = 0.0;
  double sumII = 0.0;
  double sumQQ = 0.0;
  size_t hardClipCount = 0;
  size_t nearClipCount = 0;

  for (size_t s = 0; s < samples; s++) {
    const uint8_t iByte = iq[2 * s];
    const uint8_t qByte = iq[2 * s + 1];
    const double iNorm = (static_cast<double>(iByte) - 127.5) * (1.0 / 127.5);
    const double qNorm = (static_cast<double>(qByte) - 127.5) * (1.0 / 127.5);

    sumI += iNorm;
    sumQ += qNorm;
    sumII += iNorm * iNorm;
    sumQQ += qNorm * qNorm;

    if (iByte <= 1 || iByte >= 254 || qByte <= 1 || qByte >= 254) {
      hardClipCount += 2;
    }
    if (iByte <= 8 || iByte >= 247 || qByte <= 8 || qByte >= 247) {
      nearClipCount += 2;
    }
  }

  const double n = static_cast<double>(samples);
  const double meanI = sumI / n;
  const double meanQ = sumQ / n;
  const double varI = std::max(0.0, (sumII / n) - (meanI * meanI));
  const double varQ = std::max(0.0, (sumQQ / n) - (meanQ * meanQ));
  const double rms = std::sqrt(std::max(1e-15, 0.5 * (varI + varQ)));

  out.dbfs = 20.0 * std::log10(rms + 1e-12);
  out.compensatedDbfs = out.dbfs -
                        (static_cast<double>(appliedGainDb) * gainCompFactor) +
                        signalBiasDb;

  const double safeCeil = std::max(ceilDbfs, floorDbfs + 1.0);
  const double norm =
      (out.compensatedDbfs - floorDbfs) / (safeCeil - floorDbfs);
  out.level120 = std::clamp(static_cast<float>(norm * 120.0), 0.0f, 120.0f);

  const double iqValues = static_cast<double>(samples * 2);
  out.hardClipRatio =
      (iqValues > 0.0) ? (static_cast<double>(hardClipCount) / iqValues) : 0.0;
  out.nearClipRatio =
      (iqValues > 0.0) ? (static_cast<double>(nearClipCount) / iqValues) : 0.0;
  return out;
}

float smoothSignalLevel(float input, SignalLevelSmoother &state) {
  if (!state.initialized) {
    state.value = input;
    state.initialized = true;
    return state.value;
  }
  const float alpha = (input > state.value) ? 0.42f : 0.18f;
  state.value += (input - state.value) * alpha;
  return state.value;
}
