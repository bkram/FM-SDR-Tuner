#ifndef RDS_DECODER_H
#define RDS_DECODER_H

#include <stddef.h>
#include <stdint.h>
#include <functional>
#include <memory>

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
    ~RDSDecoder();

    void reset();
    void process(const float* mpx, size_t numSamples, const std::function<void(const RDSGroup&)>& onGroup);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

#endif
