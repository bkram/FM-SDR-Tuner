#include "rds_worker.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

namespace {
constexpr size_t kQueueLimit = 32;
}

RdsWorker::RdsWorker(int inputRate, GroupCallback onGroup)
    : m_inputRate(std::max(1, inputRate)), m_onGroup(std::move(onGroup)),
      m_stop(false), m_reset(false) {}

RdsWorker::~RdsWorker() { stop(); }

void RdsWorker::start() {
  if (m_thread.joinable()) {
    return;
  }
  m_stop = false;
  m_thread = std::thread(&RdsWorker::run, this);
}

void RdsWorker::stop() {
  m_stop = true;
  m_cv.notify_all();
  if (m_thread.joinable()) {
    m_thread.join();
  }
}

void RdsWorker::enqueue(const float *samples, size_t count) {
  if (!samples || count == 0) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_queue.size() >= kQueueLimit) {
      // Drop the newest block under overload and remember the gap. The bit
      // stream is not continuous across it: one 8192-sample block at 256 kHz
      // is ~38 RDS bits, not a whole number of 26-bit blocks, so a decoder fed
      // straight across the gap keeps a false block sync until most of the
      // last 50 blocks have failed - about 1 s of lost RDS, with a few
      // miscorrected blocks on the way.
      m_gapPending = true;
      return;
    }
    m_queue.push_back(QueuedBlock{std::vector<float>(count), m_gapPending});
    m_gapPending = false;
    std::memcpy(m_queue.back().samples.data(), samples, count * sizeof(float));
  }
  m_cv.notify_one();
}

void RdsWorker::requestReset() {
  m_reset = true;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_queue.clear();
    m_gapPending = false;
  }
  m_cv.notify_one();
}

void RdsWorker::run() {
  RDSDecoder rds(m_inputRate);
  while (!m_stop.load()) {
    std::vector<float> block;
    bool doReset = false;
    bool afterGap = false;

    {
      std::unique_lock<std::mutex> lock(m_mutex);
      m_cv.wait_for(lock, std::chrono::milliseconds(50), [&]() {
        return m_stop.load() || m_reset.load() || !m_queue.empty();
      });

      if (m_stop.load()) {
        break;
      }

      doReset = m_reset.exchange(false);
      if (!m_queue.empty()) {
        block = std::move(m_queue.front().samples);
        afterGap = m_queue.front().afterGap;
        m_queue.erase(m_queue.begin());
      }
    }

    // A block that follows dropped samples restarts the decoder, so block
    // sync is re-acquired within a few blocks instead of held falsely.
    if (doReset || afterGap) {
      rds.reset();
    }

    if (!block.empty()) {
      rds.process(block.data(), block.size(), m_onGroup);
    }
  }
}
