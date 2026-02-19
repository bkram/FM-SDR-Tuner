#include "rds_decoder.h"
#include "cpu_features.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#include <immintrin.h>
#endif
#if defined(__aarch64__) || defined(__arm__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif

namespace {
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#if defined(__has_attribute)
#if __has_attribute(target)
#define FMTUNER_RDS_HAS_AVX2_KERNEL 1
#define FMTUNER_RDS_AVX2_TARGET __attribute__((target("avx2,fma")))
#endif
#elif defined(__GNUC__)
#define FMTUNER_RDS_HAS_AVX2_KERNEL 1
#define FMTUNER_RDS_AVX2_TARGET __attribute__((target("avx2,fma")))
#endif
#endif
#ifndef FMTUNER_RDS_HAS_AVX2_KERNEL
#define FMTUNER_RDS_HAS_AVX2_KERNEL 0
#define FMTUNER_RDS_AVX2_TARGET
#endif

constexpr float kPi = 3.14159265358979323846f;
constexpr int kRdsOutputRate = 19000;
constexpr int kRdsSps = 16;  // 19000 / 1187.5
constexpr uint16_t kLfsrPoly = 0b0110111001;
constexpr uint16_t kInPoly = 0b1100011011;
constexpr int kBlockLen = 26;
constexpr int kDataLen = 16;
constexpr int kPolyLen = 10;

constexpr std::array<uint16_t, 5> kSyndromes = {
    0b1111011000,  // A
    0b1111010100,  // B
    0b1001011100,  // C
    0b1111001100,  // C'
    0b1001011000   // D
};

constexpr std::array<uint16_t, 5> kOffsets = {
    0b0011111100,  // A
    0b0110011000,  // B
    0b0101101000,  // C
    0b1101010000,  // C'
    0b0110110100   // D
};

float dotProductScalar(const float* a, const float* b, size_t n) {
    float sum = 0.0f;
    for (size_t i = 0; i < n; i++) {
        sum += a[i] * b[i];
    }
    return sum;
}

#if defined(__aarch64__) || defined(__arm__) || defined(_M_ARM64)
float dotProductNeon(const float* a, const float* b, size_t n) {
    float32x4_t sumv = vdupq_n_f32(0.0f);
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        const float32x4_t av = vld1q_f32(a + i);
        const float32x4_t bv = vld1q_f32(b + i);
        sumv = vmlaq_f32(sumv, av, bv);
    }
#if defined(__aarch64__)
    float sum = vaddvq_f32(sumv);
#else
    float32x2_t sum2 = vadd_f32(vget_low_f32(sumv), vget_high_f32(sumv));
    sum2 = vpadd_f32(sum2, sum2);
    float sum = vget_lane_f32(sum2, 0);
#endif
    for (; i < n; i++) {
        sum += a[i] * b[i];
    }
    return sum;
}
#endif

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
float dotProductSse(const float* a, const float* b, size_t n) {
    __m128 sumv = _mm_setzero_ps();
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        const __m128 av = _mm_loadu_ps(a + i);
        const __m128 bv = _mm_loadu_ps(b + i);
        sumv = _mm_add_ps(sumv, _mm_mul_ps(av, bv));
    }
    alignas(16) float lanes[4];
    _mm_store_ps(lanes, sumv);
    float sum = lanes[0] + lanes[1] + lanes[2] + lanes[3];
    for (; i < n; i++) {
        sum += a[i] * b[i];
    }
    return sum;
}

#if FMTUNER_RDS_HAS_AVX2_KERNEL
FMTUNER_RDS_AVX2_TARGET float dotProductAvx2Fma(const float* a, const float* b, size_t n) {
    __m256 sumv = _mm256_setzero_ps();
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        const __m256 av = _mm256_loadu_ps(a + i);
        const __m256 bv = _mm256_loadu_ps(b + i);
        sumv = _mm256_fmadd_ps(av, bv, sumv);
    }
    alignas(32) float lanes[8];
    _mm256_store_ps(lanes, sumv);
    float sum = lanes[0] + lanes[1] + lanes[2] + lanes[3] + lanes[4] + lanes[5] + lanes[6] + lanes[7];
    for (; i < n; i++) {
        sum += a[i] * b[i];
    }
    return sum;
}
#endif
#endif
}  // namespace

RDSDecoder::RDSDecoder(int inputRate)
    : m_inputRate(std::max(1, inputRate))
    , m_pllPhase(0.0f)
    , m_pllFreq(0.0f)
    , m_pllMinFreq(0.0f)
    , m_pllMaxFreq(0.0f)
    , m_pllAlpha(0.01f)
    , m_pllBeta(0.0001f)
    , m_decimFactor(4)
    , m_baseRate(std::max(1, m_inputRate / 4))
    , m_decimPhase(0)
    , m_decimAccI(0.0f)
    , m_decimAccQ(0.0f)
    , m_rdsIHistPos(0)
    , m_rdsQHistPos(0)
    , m_pilotHistPos(0)
    , m_useNeon(false)
    , m_useSse2(false)
    , m_useAvx2(false)
    , m_resamplePos(0.0)
    , m_resampleStep(static_cast<double>(std::max(1, m_inputRate / 4)) / static_cast<double>(kRdsOutputRate))
    , m_samplePhase(0)
    , m_symbolPhase(0)
    , m_phaseWindowCount(0)
    , m_rdsAgc(1e-3f)
    , m_rdsAgcAttack(0.995f)
    , m_rdsAgcRelease(0.9995f)
    , m_prevRawBit(0)
    , m_shiftReg(0)
    , m_sync(0)
    , m_skip(0)
    , m_lastType(BLOCK_TYPE_A)
    , m_contGroup(0)
    , m_rdsLocked(false)
    , m_goodGroupRun(0)
    , m_badGroupRun(0)
    , m_samplesSinceGoodGroup(0)
    , m_lockAcquireGroups(2)
    , m_lockLossGroups(12)
    , m_aggressiveness(Aggressiveness::Balanced) {
    m_rdsTaps = designLowPass(2400.0, 2400.0, static_cast<double>(m_baseRate));
    m_rdsTapsRev = m_rdsTaps;
    std::reverse(m_rdsTapsRev.begin(), m_rdsTapsRev.end());
    m_rdsIHistory.assign(m_rdsTapsRev.size() * 2, 0.0f);
    m_rdsQHistory.assign(m_rdsTapsRev.size() * 2, 0.0f);

    // Pilot bandpass filter widened for stronger lock robustness on noisy/weak RTL frontends.
    m_pilotTaps = designBandPass(18000.0, 20000.0, 2000.0, static_cast<double>(m_inputRate));
    m_pilotTapsRev = m_pilotTaps;
    std::reverse(m_pilotTapsRev.begin(), m_pilotTapsRev.end());
    m_pilotHistory.assign(m_pilotTapsRev.size() * 2, 0.0f);

    m_pllFreq = 2.0f * kPi * 19000.0f / static_cast<float>(m_inputRate);
    m_pllMinFreq = 2.0f * kPi * 18750.0f / static_cast<float>(m_inputRate);
    m_pllMaxFreq = 2.0f * kPi * 19250.0f / static_cast<float>(m_inputRate);
    m_resampleStep = static_cast<double>(m_baseRate) / static_cast<double>(kRdsOutputRate);
    const CPUFeatures cpu = detectCPUFeatures();
    m_useNeon = cpu.neon;
    m_useSse2 = cpu.sse2;
    m_useAvx2 = cpu.avx2 && cpu.fma;
    m_iBuf.reserve(16384);
    m_qBuf.reserve(16384);
    reset();
}

void RDSDecoder::reset() {
    m_pllPhase = 0.0f;
    m_pllFreq = 2.0f * kPi * 19000.0f / static_cast<float>(m_inputRate);
    m_decimPhase = 0;
    m_decimAccI = 0.0f;
    m_decimAccQ = 0.0f;
    m_rdsIHistPos = 0;
    m_rdsQHistPos = 0;
    m_iBuf.clear();
    m_qBuf.clear();
    m_resamplePos = 0.0;
    m_samplePhase = 0;
    m_symbolPhase = 0;
    std::fill(std::begin(m_phaseEnergy), std::end(m_phaseEnergy), 0.0f);
    m_phaseWindowCount = 0;
    m_rdsAgc = 1e-3f;
    m_prevRawBit = 0;

    m_shiftReg = 0;
    m_sync = 0;
    m_skip = 0;
    m_lastType = BLOCK_TYPE_A;
    m_contGroup = 0;
    std::memset(m_blocks, 0, sizeof(m_blocks));
    std::memset(m_blockAvail, 0, sizeof(m_blockAvail));
    std::memset(m_blockErrors, 3, sizeof(m_blockErrors));
    m_rdsLocked = false;
    m_goodGroupRun = 0;
    m_badGroupRun = 0;
    m_samplesSinceGoodGroup = 0;

    std::fill(m_rdsIHistory.begin(), m_rdsIHistory.end(), 0.0f);
    std::fill(m_rdsQHistory.begin(), m_rdsQHistory.end(), 0.0f);
    std::fill(m_pilotHistory.begin(), m_pilotHistory.end(), 0.0f);
}

void RDSDecoder::setLockThresholds(int acquireGroups, int lossGroups) {
    m_lockAcquireGroups = std::clamp(acquireGroups, 1, 16);
    m_lockLossGroups = std::clamp(lossGroups, 1, 64);
}

void RDSDecoder::setAgcCoefficients(float attack, float release) {
    m_rdsAgcAttack = std::clamp(attack, 0.90f, 0.99995f);
    m_rdsAgcRelease = std::clamp(release, 0.90f, 0.99999f);
}

void RDSDecoder::process(const float* mpx, size_t numSamples, const std::function<void(const RDSGroup&)>& onGroup) {
    if (!mpx || numSamples == 0) {
        return;
    }

    const int lockTimeoutSamples = std::max(1, m_inputRate) * 2;
    m_samplesSinceGoodGroup = std::min(m_samplesSinceGoodGroup + static_cast<int>(numSamples), lockTimeoutSamples);
    if (m_rdsLocked && m_samplesSinceGoodGroup >= lockTimeoutSamples) {
        m_rdsLocked = false;
        m_goodGroupRun = 0;
        m_badGroupRun = 0;
    }

    for (size_t i = 0; i < numSamples; i++) {
        const float x = mpx[i];

        // PLL driven from pilot-filtered MPX for better lock in noise.
        const float pilot = filterSample(x, m_pilotTapsRev, m_pilotHistory, m_pilotHistPos);
        const float vcoI = std::cos(m_pllPhase);
        const float vcoQ = std::sin(m_pllPhase);
        const float error = pilot * vcoQ;
        m_pllFreq = std::clamp(m_pllFreq + (m_pllBeta * error), m_pllMinFreq, m_pllMaxFreq);
        m_pllPhase += m_pllFreq + (m_pllAlpha * error);
        if (m_pllPhase > 2.0f * kPi) {
            m_pllPhase -= 2.0f * kPi;
        } else if (m_pllPhase < 0.0f) {
            m_pllPhase += 2.0f * kPi;
        }

        // Coherent 57 kHz downmix from pilot PLL.
        const float phase57 = 3.0f * m_pllPhase;
        const float loI = std::cos(phase57);
        const float loQ = -std::sin(phase57);
        m_decimAccI += x * loI;
        m_decimAccQ += x * loQ;
        if (++m_decimPhase < m_decimFactor) {
            continue;
        }
        m_decimPhase = 0;

        const float mixedI = m_decimAccI / static_cast<float>(m_decimFactor);
        const float mixedQ = m_decimAccQ / static_cast<float>(m_decimFactor);
        m_decimAccI = 0.0f;
        m_decimAccQ = 0.0f;

        const float baseI = filterSample(mixedI, m_rdsTapsRev, m_rdsIHistory, m_rdsIHistPos);
        const float baseQ = filterSample(mixedQ, m_rdsTapsRev, m_rdsQHistory, m_rdsQHistPos);
        m_iBuf.push_back(baseI);
        m_qBuf.push_back(baseQ);
    }

    while (m_resamplePos + 1.0 < static_cast<double>(m_iBuf.size())) {
        const size_t i0 = static_cast<size_t>(m_resamplePos);
        const size_t i1 = i0 + 1;
        const float frac = static_cast<float>(m_resamplePos - static_cast<double>(i0));
        const float iVal = m_iBuf[i0] + ((m_iBuf[i1] - m_iBuf[i0]) * frac);
        const float qVal = m_qBuf[i0] + ((m_qBuf[i1] - m_qBuf[i0]) * frac);
        const float mag = std::sqrt((iVal * iVal) + (qVal * qVal));
        const float agcCoeff = (mag > m_rdsAgc) ? m_rdsAgcAttack : m_rdsAgcRelease;
        m_rdsAgc = (m_rdsAgc * agcCoeff) + (mag * (1.0f - agcCoeff));
        const float normI = iVal / std::max(m_rdsAgc, 1e-4f);

        m_phaseEnergy[m_samplePhase] = (m_phaseEnergy[m_samplePhase] * 0.995f) + (std::abs(normI) * 0.005f);
        m_samplePhase = (m_samplePhase + 1) % kRdsSps;
        if (++m_phaseWindowCount >= (kRdsSps * 64)) {
            int bestPhase = m_symbolPhase;
            float bestEnergy = m_phaseEnergy[bestPhase];
            for (int p = 0; p < kRdsSps; p++) {
                if (m_phaseEnergy[p] > bestEnergy) {
                    bestEnergy = m_phaseEnergy[p];
                    bestPhase = p;
                }
            }
            m_symbolPhase = bestPhase;
            m_phaseWindowCount = 0;
        }

        if (((m_samplePhase + kRdsSps - 1) % kRdsSps) == m_symbolPhase) {
            const uint8_t rawBit = (normI >= 0.0f) ? 1 : 0;
            const uint8_t diffBit = rawBit ^ m_prevRawBit;
            m_prevRawBit = rawBit;
            processBit(diffBit, onGroup);
        }

        m_resamplePos += m_resampleStep;
    }

    const size_t consumed = static_cast<size_t>(m_resamplePos);
    if (consumed > 0) {
        m_iBuf.erase(m_iBuf.begin(), m_iBuf.begin() + static_cast<std::ptrdiff_t>(consumed));
        m_qBuf.erase(m_qBuf.begin(), m_qBuf.begin() + static_cast<std::ptrdiff_t>(consumed));
        m_resamplePos -= static_cast<double>(consumed);
    }
}

void RDSDecoder::processBit(uint8_t bit, const std::function<void(const RDSGroup&)>& onGroup) {
    m_shiftReg = ((m_shiftReg << 1) & 0x3FFFFFFu) | (bit & 1u);
    if (--m_skip > 0) {
        return;
    }

    const uint16_t syn = calcSyndrome(m_shiftReg);
    bool known = false;
    BlockType type = BLOCK_TYPE_A;
    for (int i = 0; i < static_cast<int>(kSyndromes.size()); i++) {
        if (syn == kSyndromes[static_cast<size_t>(i)]) {
            known = true;
            type = static_cast<BlockType>(i);
            break;
        }
    }

    m_sync = std::clamp(known ? (m_sync + 1) : (m_sync - 1), 0, 4);
    if (!m_sync) {
        return;
    }

    if (!known) {
        static constexpr BlockType kNextType[BLOCK_TYPE_COUNT] = {
            BLOCK_TYPE_B,   // after A -> B
            BLOCK_TYPE_C,   // after B -> C (or C')
            BLOCK_TYPE_D,   // after C -> D
            BLOCK_TYPE_D,   // after C' -> D
            BLOCK_TYPE_A    // after D -> A
        };
        type = kNextType[static_cast<int>(m_lastType)];
    }

    uint8_t errorLevel = 3;
    m_blocks[type] = correctErrors(m_shiftReg, type, errorLevel);
    if (!known && errorLevel < 3) {
        // Inferred block type is less trustworthy than a syndrome-matched one.
        errorLevel = std::max<uint8_t>(errorLevel, 2);
    }
    m_blockErrors[type] = errorLevel;
    m_blockAvail[type] = (errorLevel < 3);

    if (type == BLOCK_TYPE_A) {
        // Reset group tracking on block A.
        m_contGroup = 0;
    } else if (type == BLOCK_TYPE_B) {
        m_contGroup = 1;
    } else if ((type == BLOCK_TYPE_C || type == BLOCK_TYPE_CP) && m_lastType == BLOCK_TYPE_B) {
        m_contGroup++;
    } else if (type == BLOCK_TYPE_D && (m_lastType == BLOCK_TYPE_C || m_lastType == BLOCK_TYPE_CP)) {
        m_contGroup++;
    } else {
        m_contGroup = 0;
    }

    if (m_contGroup >= 3) {
        const uint16_t a = static_cast<uint16_t>((m_blocks[BLOCK_TYPE_A] >> 10) & 0xFFFFu);
        const uint16_t b = static_cast<uint16_t>((m_blocks[BLOCK_TYPE_B] >> 10) & 0xFFFFu);
        const bool useCp = m_blockAvail[BLOCK_TYPE_CP] && !m_blockAvail[BLOCK_TYPE_C];
        const uint16_t c = static_cast<uint16_t>(((useCp ? m_blocks[BLOCK_TYPE_CP] : m_blocks[BLOCK_TYPE_C]) >> 10) & 0xFFFFu);
        const uint16_t d = static_cast<uint16_t>((m_blocks[BLOCK_TYPE_D] >> 10) & 0xFFFFu);
        const bool cValid = m_blockAvail[BLOCK_TYPE_C] || m_blockAvail[BLOCK_TYPE_CP];
        const bool goodGroup = m_blockAvail[BLOCK_TYPE_A] && m_blockAvail[BLOCK_TYPE_B] && cValid && m_blockAvail[BLOCK_TYPE_D];
        int acquireNeed = m_lockAcquireGroups;
        int lossNeed = m_lockLossGroups;
        if (m_aggressiveness == Aggressiveness::Strict) {
            acquireNeed = 5;
            lossNeed = 5;
        } else if (m_aggressiveness == Aggressiveness::Loose) {
            acquireNeed = 2;
            lossNeed = 12;
        }

        if (goodGroup) {
            m_samplesSinceGoodGroup = 0;
            m_goodGroupRun = std::min(m_goodGroupRun + 1, acquireNeed * 2);
            m_badGroupRun = 0;
            if (!m_rdsLocked && m_goodGroupRun >= acquireNeed) {
                m_rdsLocked = true;
            }
        } else {
            m_badGroupRun = std::min(m_badGroupRun + 1, lossNeed * 2);
            if (m_goodGroupRun > 0) {
                m_goodGroupRun--;
            }
            if (m_rdsLocked && m_badGroupRun >= lossNeed) {
                m_rdsLocked = false;
            }
        }

        const uint8_t ea = m_blockErrors[BLOCK_TYPE_A];
        const uint8_t eb = m_blockErrors[BLOCK_TYPE_B];
        const uint8_t ec = useCp ? m_blockErrors[BLOCK_TYPE_CP] : m_blockErrors[BLOCK_TYPE_C];
        const uint8_t ed = m_blockErrors[BLOCK_TYPE_D];
        const uint8_t errors = static_cast<uint8_t>((ea << 6) | (eb << 4) | (ec << 2) | ed);

        if (onGroup) {
            onGroup(RDSGroup{a, b, c, d, errors});
        }
        m_contGroup = 0;
    }

    m_lastType = type;
    m_skip = kBlockLen;
}

uint16_t RDSDecoder::calcSyndrome(uint32_t block) const {
    uint16_t syn = 0;
    for (int i = kBlockLen - 1; i >= 0; i--) {
        const uint8_t outBit = (syn >> (kPolyLen - 1)) & 1u;
        syn = static_cast<uint16_t>((syn << 1) & 0x3FFu);
        syn ^= static_cast<uint16_t>(kLfsrPoly * outBit);
        syn ^= static_cast<uint16_t>(kInPoly * ((block >> i) & 1u));
    }
    return syn;
}

uint32_t RDSDecoder::correctErrors(uint32_t block, BlockType type, uint8_t& errorLevel) const {
    uint32_t raw = block ^ static_cast<uint32_t>(kOffsets[static_cast<size_t>(type)]);
    uint32_t out = raw;
    uint16_t syn = calcSyndrome(raw);
    const bool hadError = (syn != 0);

    uint8_t errorFound = 0;
    if (syn) {
        for (int i = kDataLen - 1; i >= 0; i--) {
            errorFound |= !(syn & 0b11111);
            const uint8_t outBit = (syn >> (kPolyLen - 1)) & 1u;
            out ^= static_cast<uint32_t>(errorFound & outBit) << static_cast<uint32_t>(i + kPolyLen);
            syn = static_cast<uint16_t>((syn << 1) & 0x3FFu);
            syn ^= static_cast<uint16_t>(kLfsrPoly * outBit * !errorFound);
        }
    }
    const bool recovered = !(syn & 0b11111);
    if (!recovered) {
        errorLevel = 3; // uncorrectable / unknown, let client-side parser decide.
        return raw;
    }
    errorLevel = hadError ? 1 : 0; // corrected / clean.
    return out;
}

float RDSDecoder::filterSample(float input, const std::vector<float>& taps, std::vector<float>& history, size_t& pos) {
    if (taps.empty() || history.size() < (taps.size() * 2)) {
        return input;
    }

    const size_t tapCount = taps.size();
    history[pos] = input;
    history[pos + tapCount] = input;
    pos = (pos + 1) % tapCount;

    const float* x = &history[pos];
    const float* h = taps.data();

#if defined(__aarch64__) || defined(__arm__) || defined(_M_ARM64)
    if (m_useNeon) {
        return dotProductNeon(x, h, tapCount);
    }
#endif
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#if FMTUNER_RDS_HAS_AVX2_KERNEL
    if (m_useAvx2) {
        return dotProductAvx2Fma(x, h, tapCount);
    }
#endif
    if (m_useSse2) {
        return dotProductSse(x, h, tapCount);
    }
#endif
    return dotProductScalar(x, h, tapCount);
}

std::vector<float> RDSDecoder::designLowPass(double cutoffHz, double transitionHz, double sampleRate) const {
    int tapCount = static_cast<int>(std::ceil(3.8 * sampleRate / transitionHz));
    tapCount = std::clamp(tapCount, 31, 127);
    if ((tapCount % 2) == 0) {
        tapCount++;
    }

    std::vector<float> taps(static_cast<size_t>(tapCount), 0.0f);
    const int mid = tapCount / 2;
    const double omega = 2.0 * kPi * cutoffHz / sampleRate;
    double sum = 0.0;

    for (int n = 0; n < tapCount; n++) {
        const int m = n - mid;
        const double sinc = (m == 0)
            ? (omega / kPi)
            : (std::sin(omega * static_cast<double>(m)) / (kPi * static_cast<double>(m)));
        const double h = sinc * windowNuttall(n, tapCount);
        taps[static_cast<size_t>(n)] = static_cast<float>(h);
        sum += h;
    }

    if (std::abs(sum) > 1e-12) {
        const float inv = static_cast<float>(1.0 / sum);
        for (float& tap : taps) {
            tap *= inv;
        }
    }
    return taps;
}

std::vector<float> RDSDecoder::designBandPass(double lowHz, double highHz, double transitionHz, double sampleRate) const {
    const double fs = std::max(1.0, sampleRate);
    int tapCount = static_cast<int>(std::ceil(3.8 * fs / transitionHz));
    tapCount = std::clamp(tapCount, 31, 127);
    if ((tapCount % 2) == 0) {
        tapCount++;
    }

    std::vector<float> taps(static_cast<size_t>(tapCount), 0.0f);
    const int mid = tapCount / 2;
    double sumAbs = 0.0;

    for (int n = 0; n < tapCount; n++) {
        const int m = n - mid;
        double h = 0.0;
        if (m == 0) {
            h = 2.0 * (highHz - lowHz) / fs;
        } else {
            const double mm = static_cast<double>(m);
            h = (std::sin(2.0 * kPi * highHz * mm / fs) - std::sin(2.0 * kPi * lowHz * mm / fs)) / (kPi * mm);
        }
        h *= windowNuttall(n, tapCount);
        taps[static_cast<size_t>(n)] = static_cast<float>(h);
        sumAbs += std::abs(h);
    }

    if (sumAbs > 1e-12) {
        const float norm = static_cast<float>(1.0 / sumAbs);
        for (float& tap : taps) {
            tap *= norm;
        }
    }

    return taps;
}

float RDSDecoder::windowNuttall(int n, int count) const {
    if (count <= 1) {
        return 1.0f;
    }
    const double x = 2.0 * kPi * static_cast<double>(n) / static_cast<double>(count - 1);
    return static_cast<float>(0.355768
        - 0.487396 * std::cos(x)
        + 0.144232 * std::cos(2.0 * x)
        - 0.012604 * std::cos(3.0 * x));
}
