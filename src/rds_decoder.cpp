#include "rds_decoder.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <vector>

#include "redsea_port/block_sync.hh"
#include "redsea_port/constants.hh"
#include "redsea_port/dsp/subcarrier.hh"
#include "redsea_port/io/input.hh"
#include "redsea_port/options.hh"

struct RDSDecoder::Impl {
    explicit Impl(int inputRate)
        : sampleRate(std::max(1, inputRate))
        , subcarriers(static_cast<float>(sampleRate)) {
        options.use_fec = true;
        blockStream.init(options);
    }

    void reset() {
        subcarriers.reset();
        blockStream = redsea::BlockStream();
        blockStream.init(options);
    }

    uint8_t packErrors(const redsea::Group& group) const {
        auto mapErr = [&](redsea::eBlockNumber b) -> uint8_t {
            if (!group.has(b)) {
                return 3;
            }
            return group.hadErrors(b) ? 1 : 0;
        };
        const uint8_t a = mapErr(redsea::BLOCK1);
        const uint8_t b = mapErr(redsea::BLOCK2);
        const uint8_t c = mapErr(redsea::BLOCK3);
        const uint8_t d = mapErr(redsea::BLOCK4);
        return static_cast<uint8_t>((a << 6) | (b << 4) | (c << 2) | d);
    }

    void emitGroups(const redsea::BitBuffer& bits, const std::function<void(const RDSGroup&)>& onGroup) {
        for (const redsea::TimedBit& bit : bits.bits[0]) {
            blockStream.pushBit(bit.value);
            if (!blockStream.hasGroupReady()) {
                continue;
            }
            const redsea::Group g = blockStream.popGroup();
            const uint16_t a = g.has(redsea::BLOCK1) ? g.get(redsea::BLOCK1) : 0;
            const uint16_t b = g.has(redsea::BLOCK2) ? g.get(redsea::BLOCK2) : 0;
            const uint16_t c = g.has(redsea::BLOCK3) ? g.get(redsea::BLOCK3) : 0;
            const uint16_t d = g.has(redsea::BLOCK4) ? g.get(redsea::BLOCK4) : 0;
            if (onGroup) {
                onGroup(RDSGroup{a, b, c, d, packErrors(g)});
            }
        }
    }

    int sampleRate;
    redsea::Options options;
    redsea::SubcarrierSet subcarriers;
    redsea::BlockStream blockStream;
};

RDSDecoder::RDSDecoder(int inputRate)
    : m_impl(std::make_unique<Impl>(inputRate)) {}

RDSDecoder::~RDSDecoder() = default;

void RDSDecoder::reset() {
    m_impl->reset();
}

void RDSDecoder::process(const float* mpx, size_t numSamples, const std::function<void(const RDSGroup&)>& onGroup) {
    if (!mpx || numSamples == 0) {
        return;
    }

    size_t offset = 0;
    while (offset < numSamples) {
        const size_t chunk = std::min(static_cast<size_t>(redsea::kInputChunkSize), numSamples - offset);
        redsea::MPXBuffer input{};
        input.used_size = chunk;
        input.time_received = std::chrono::system_clock::now();
        std::memcpy(input.data.data(), mpx + offset, chunk * sizeof(float));

        const redsea::BitBuffer bits = m_impl->subcarriers.chunkToBits(input, 1);
        m_impl->emitGroups(bits, onGroup);
        offset += chunk;
    }
}
