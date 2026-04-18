#ifndef RDS_WORKER_H
#define RDS_WORKER_H

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "rds_decoder.h"

class RdsWorker {
public:
  using GroupCallback = std::function<void(const RDSGroup &)>;

  explicit RdsWorker(int inputRate, GroupCallback onGroup);
  ~RdsWorker();

  void start();
  void stop();
  void enqueue(const float *samples, size_t count);
  void requestReset();

private:
  void run();

  int m_inputRate;
  GroupCallback m_onGroup;
  std::atomic<bool> m_stop;
  std::atomic<bool> m_reset;
  std::mutex m_mutex;
  std::condition_variable m_cv;
  std::vector<std::vector<float>> m_queue;
  std::thread m_thread;
};

#endif
