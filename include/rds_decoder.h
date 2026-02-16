#ifndef RDS_DECODER_H
#define RDS_DECODER_H

#include <stddef.h>
#include <stdint.h>
#include <functional>
#include <vector>

struct RDSGroup {
    uint16_t blockA;
    uint16_t blockB;
    uint16_t blockC;
    uint16_t blockD;
    uint8_t errors;
};

class RDSDecoder {
public:
    explicit RDSDecoder(int inputRate);

    void reset();
    void process(const float* mpx, size_t numSamples, const std::function<void(const RDSGroup&)>& onGroup);

private:
    enum BlockType {
        BLOCK_TYPE_A = 0,
        BLOCK_TYPE_B = 1,
        BLOCK_TYPE_C = 2,
        BLOCK_TYPE_CP = 3,
        BLOCK_TYPE_D = 4,
        BLOCK_TYPE_COUNT = 5
    };

    float filterSample(float input, const std::vector<float>& taps, std::vector<float>& history, size_t& pos);
    std::vector<float> designLowPass(double cutoffHz, double transitionHz, double sampleRate) const;
    float windowNuttall(int n, int count) const;

    uint16_t calcSyndrome(uint32_t block) const;
    uint32_t correctErrors(uint32_t block, BlockType type, bool& recovered) const;
    void processBit(uint8_t bit, const std::function<void(const RDSGroup&)>& onGroup);

    int m_inputRate;

    // Pilot PLL (for 57 kHz coherent downmix).
    float m_pllPhase;
    float m_pllFreq;
    float m_pllMinFreq;
    float m_pllMaxFreq;
    float m_pllAlpha;
    float m_pllBeta;

    // RDS baseband filters.
    int m_decimFactor;
    int m_baseRate;
    int m_decimPhase;
    float m_decimAccI;
    float m_decimAccQ;
    std::vector<float> m_rdsTaps;
    std::vector<float> m_rdsTapsRev;
    std::vector<float> m_rdsIHistory;
    std::vector<float> m_rdsQHistory;
    size_t m_rdsIHistPos;
    size_t m_rdsQHistPos;
    bool m_useNeon;
    bool m_useSse2;
    bool m_useAvx2;

    // 256k -> 19k linear resampler state.
    std::vector<float> m_iBuf;
    std::vector<float> m_qBuf;
    double m_resamplePos;
    double m_resampleStep;

    // Bit timing and slicing.
    int m_samplePhase;
    int m_symbolPhase;
    float m_phaseEnergy[16];
    int m_phaseWindowCount;
    float m_agc;
    uint8_t m_prevRawBit;

    // RDS framer/synchronizer.
    uint32_t m_shiftReg;
    int m_sync;
    int m_skip;
    BlockType m_lastType;
    int m_contGroup;
    uint32_t m_blocks[BLOCK_TYPE_COUNT];
    bool m_blockAvail[BLOCK_TYPE_COUNT];

    // Debounced RDS lock tracking to avoid short status flaps.
    bool m_rdsLocked;
    int m_goodGroupRun;
    int m_badGroupRun;
    int m_samplesSinceGoodGroup;
};

#endif
