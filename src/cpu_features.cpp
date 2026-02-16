#include "cpu_features.h"

#include <sstream>

CPUFeatures detectCPUFeatures() {
    CPUFeatures out;

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
    out.isX86 = true;
#if defined(__GNUC__) || defined(__clang__)
    __builtin_cpu_init();
    out.sse2 = __builtin_cpu_supports("sse2");
    out.sse41 = __builtin_cpu_supports("sse4.1");
    out.avx = __builtin_cpu_supports("avx");
    out.avx2 = __builtin_cpu_supports("avx2");
    out.fma = __builtin_cpu_supports("fma");
#endif
#endif

#if defined(__aarch64__) || defined(__arm__) || defined(_M_ARM64)
    out.isArm = true;
    // On Apple Silicon and most modern ARM targets used for SDR, NEON is available.
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
            << " sse2=" << (sse2 ? 1 : 0)
            << " sse4.1=" << (sse41 ? 1 : 0)
            << " avx=" << (avx ? 1 : 0)
            << " avx2=" << (avx2 ? 1 : 0)
            << " fma=" << (fma ? 1 : 0);
    } else if (isArm) {
        oss << "arch=arm"
            << " neon=" << (neon ? 1 : 0);
    } else {
        oss << "arch=unknown";
    }
    return oss.str();
}
