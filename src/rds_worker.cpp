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
      // Keep continuity for decoder lock; drop newest block under overload.
      return;
    }
    m_queue.emplace_back(count);
    std::memcpy(m_queue.back().data(), samples, count * sizeof(float));
  }
  m_cv.notify_one();
}

void RdsWorker::requestReset() {
  m_reset = true;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_queue.clear();
  }
  m_cv.notify_one();
}

void RdsWorker::run() {
  RDSDecoder rds(m_inputRate);
  while (!m_stop.load()) {
    std::vector<float> block;
    bool doReset = false;

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
        block = std::move(m_queue.front());
        m_queue.erase(m_queue.begin());
      }
    }

    if (doReset) {
      rds.reset();
    }

    if (!block.empty()) {
      rds.process(block.data(), block.size(), m_onGroup);
    }
  }
}
