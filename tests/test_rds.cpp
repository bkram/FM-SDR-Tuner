#include "catch_compat.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#define private public
#include "rds_worker.h"
#undef private
#include "rds_decoder.h"
#include "rds_signal.h"

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
