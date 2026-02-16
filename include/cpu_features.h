#ifndef CPU_FEATURES_H
#define CPU_FEATURES_H

#include <string>

struct CPUFeatures {
    bool isX86 = false;
    bool isArm = false;
    bool neon = false;
    bool sse2 = false;
    bool sse41 = false;
    bool avx = false;
    bool avx2 = false;
    bool fma = false;

    std::string summary() const;
};

CPUFeatures detectCPUFeatures();

#endif
