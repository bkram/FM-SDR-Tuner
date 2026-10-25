#include "catch_compat.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#define private public
#include "rds_worker.h"
#undef private
#include "rds_decoder.h"
#include "rds_signal.h"
#include "redsea_port/block_sync.hh"
#include "redsea_port/group.hh"

namespace {

constexpr int kSampleRate = 256000;
constexpr size_t kBlockSamples = 8192;

// Groups decoded with every block error-free.
int countCleanGroups(const std::vector<RDSGroup> &groups) {
  int clean = 0;
  for (const RDSGroup &g : groups) {
    if (g.errors == 0) {
      clean++;
    }
  }
  return clean;
}

std::vector<std::vector<float>> makeMpxBlocks(double seconds) {
  const int groupCount = static_cast<int>(seconds * 1187.5 / 104.0) + 4;
  rds_signal::MpxGenerator gen(
      kSampleRate,
      rds_signal::groupsToBits(rds_signal::makeGroups(groupCount, 0x1000)));
  const size_t blockCount =
      static_cast<size_t>(seconds * kSampleRate) / kBlockSamples;
  std::vector<std::vector<float>> blocks(blockCount,
                                         std::vector<float>(kBlockSamples));
  for (auto &block : blocks) {
    for (float &s : block) {
      s = gen.next();
    }
  }
  return blocks;
}

} // namespace

TEST_CASE("RDS worker recovers quickly after dropping a block on overload",
          "[rds]") {
  const auto blocks = makeMpxBlocks(12.0);
  constexpr size_t kQueueLimit = 32;

  // Reference: the same stream with the same block missing, decoded with a
  // reset at the gap. That is the best any decoder can do across the gap.
  std::vector<RDSGroup> reference;
  {
    RDSDecoder decoder(kSampleRate);
    for (size_t i = 0; i < blocks.size(); i++) {
      if (i == kQueueLimit) {
        decoder.reset();
        continue;
      }
      decoder.process(blocks[i].data(), blocks[i].size(),
                      [&](const RDSGroup &g) { reference.push_back(g); });
    }
  }

  std::mutex groupsMutex;
  std::vector<RDSGroup> decoded;
  RdsWorker worker(kSampleRate, [&](const RDSGroup &g) {
    std::lock_guard<std::mutex> lock(groupsMutex);
    decoded.push_back(g);
  });

  // With the worker not yet running the queue fills to its limit, and the
  // next block overflows it - the worker drops block kQueueLimit.
  for (size_t i = 0; i <= kQueueLimit; i++) {
    worker.enqueue(blocks[i].data(), blocks[i].size());
  }
  worker.start();

  auto queueDepth = [&]() {
    std::lock_guard<std::mutex> lock(worker.m_mutex);
    return worker.m_queue.size();
  };
  for (size_t i = kQueueLimit + 1; i < blocks.size(); i++) {
    while (queueDepth() >= kQueueLimit / 2) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    worker.enqueue(blocks[i].data(), blocks[i].size());
  }
  while (queueDepth() > 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  worker.stop();

  const int referenceClean = countCleanGroups(reference);
  int workerClean = 0;
  {
    std::lock_guard<std::mutex> lock(groupsMutex);
    workerClean = countCleanGroups(decoded);
  }
  INFO("clean groups: worker " << workerClean << ", reference "
                               << referenceClean);
  REQUIRE(referenceClean > 100);
  // Fed straight across the gap the decoder held a false block sync for
  // about a second, losing ~10 groups.
  REQUIRE(workerClean >= referenceClean - 2);
}

TEST_CASE("RDS burst correction stands down at a high block error rate",
          "[rds]") {
  // Feed the block synchroniser bits directly, with random bit errors, and
  // count the blocks it reports as corrected but whose data is wrong. The
  // groups start at bit 0, so a group that ends on a multiple of 104 bits
  // tells exactly which one was sent; one that does not came from a false
  // sync and is not counted.
  struct Result {
    int correctedRight = 0;
    int correctedWrong = 0;
    int blocks = 0;
  };
  auto run = [](double bitErrorRate) {
    const std::vector<rds_signal::Group> sent =
        rds_signal::makeGroups(3000, 0x2000);
    std::vector<int> bits = rds_signal::groupsToBits(sent);
    std::mt19937 rng(7);
    std::bernoulli_distribution flip(bitErrorRate);
    for (int &b : bits) {
      if (flip(rng)) {
        b ^= 1;
      }
    }

    redsea::Options options;
    options.use_fec = true;
    redsea::BlockStream stream;
    stream.init(options);

    Result r;
    size_t pushed = 0;
    for (int bit : bits) {
      stream.pushBit(bit != 0);
      pushed++;
      if (!stream.hasGroupReady()) {
        continue;
      }
      const redsea::Group g = stream.popGroup();
      if (pushed % 104 != 0) {
        continue;
      }
      const size_t index = pushed / 104 - 1;
      const rds_signal::Group &s = sent[static_cast<size_t>(index)];
      const uint16_t want[4] = {s.a, s.b, s.c, s.d};
      const redsea::eBlockNumber num[4] = {redsea::BLOCK1, redsea::BLOCK2,
                                           redsea::BLOCK3, redsea::BLOCK4};
      for (int k = 0; k < 4; k++) {
        r.blocks++;
        if (!g.has(num[k]) || !g.hadErrors(num[k])) {
          continue;
        }
        if (g.get(num[k]) == want[k]) {
          r.correctedRight++;
        } else {
          r.correctedWrong++;
        }
      }
    }
    return r;
  };

  // A good signal (~5% BLER): correction still repairs the odd bad block.
  const Result good = run(0.002);
  INFO("good: right " << good.correctedRight << " wrong "
                      << good.correctedWrong);
  REQUIRE(good.correctedRight > 50);
  REQUIRE(good.correctedWrong <= good.correctedRight / 50);

  // A poor signal (~65% and ~75% BLER, well above the 50% gate): a corrected
  // block must be trusted, so wrong corrections have to stay rare.
  for (double ber : {0.04, 0.05}) {
    const Result poor = run(ber);
    INFO("BER " << ber << ": blocks " << poor.blocks << " right "
                << poor.correctedRight << " wrong " << poor.correctedWrong);
    REQUIRE(poor.blocks > 1000);
    REQUIRE(poor.correctedWrong <= poor.blocks / 500);
  }
}
