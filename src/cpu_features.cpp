#include "cpu_features.h"

#include <sstream>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <immintrin.h>
#include <intrin.h>
#endif

CPUFeatures detectCPUFeatures() {
  CPUFeatures out;

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) ||             \
    defined(_M_IX86)
  out.isX86 = true;
#if defined(__GNUC__) || defined(__clang__)
  __builtin_cpu_init();
  out.sse2 = __builtin_cpu_supports("sse2");
  out.sse41 = __builtin_cpu_supports("sse4.1");
  out.avx = __builtin_cpu_supports("avx");
  out.avx2 = __builtin_cpu_supports("avx2");
  out.fma = __builtin_cpu_supports("fma");
#elif defined(_MSC_VER)
  int regs[4] = {0, 0, 0, 0};
  __cpuid(regs, 1);
  out.sse2 = (regs[3] & (1 << 26)) != 0;
  out.sse41 = (regs[2] & (1 << 19)) != 0;
  const bool osxsave = (regs[2] & (1 << 27)) != 0;
  const bool cpuAvx = (regs[2] & (1 << 28)) != 0;
  const bool cpuFma = (regs[2] & (1 << 12)) != 0;
  bool osYmm = false;
  if (osxsave && cpuAvx) {
    const unsigned long long xcr0 = _xgetbv(0);
    osYmm = (xcr0 & 0x6ULL) == 0x6ULL;
  }
  out.avx = cpuAvx && osYmm;
  out.fma = cpuFma && out.avx;
  if (out.avx) {
    int regs7[4] = {0, 0, 0, 0};
    __cpuidex(regs7, 7, 0);
    out.avx2 = (regs7[1] & (1 << 5)) != 0;
  }
#endif
#endif

#if defined(__aarch64__) || defined(__arm__) || defined(_M_ARM64)
  out.isArm = true;
  // On Apple Silicon and most modern ARM targets used for SDR, NEON is
  // available.
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
  out.neon = true;
#elif defined(__aarch64__)
  out.neon = true;
#endif
#endif

  return out;
}

std::string CPUFeatures::summary() const {
  std::ostringstream oss;
  if (isX86) {
    oss << "arch=x86"
        << " sse2=" << (sse2 ? 1 : 0) << " sse4.1=" << (sse41 ? 1 : 0)
        << " avx=" << (avx ? 1 : 0) << " avx2=" << (avx2 ? 1 : 0)
        << " fma=" << (fma ? 1 : 0);
  } else if (isArm) {
    oss << "arch=arm"
        << " neon=" << (neon ? 1 : 0);
  } else {
    oss << "arch=unknown";
  }
  return oss.str();
}
