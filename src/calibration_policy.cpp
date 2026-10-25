#include "calibration.h"

#include "signal_level.h"

#include <algorithm>
#include <cmath>

namespace fm_tuner::calibration {

std::vector<uint32_t> makeCaptureCenters(uint32_t startHz, uint32_t endHz,
                                         uint32_t gridStepHz,
                                         uint32_t captureRateHz,
                                         uint32_t measurementWindowHz) {
  std::vector<uint32_t> centers;
  if (startHz > endHz || gridStepHz == 0 || captureRateHz == 0 ||
      measurementWindowHz >= captureRateHz) {
    return centers;
  }

  const int64_t usableHalfSpan =
      static_cast<int64_t>(static_cast<double>(captureRateHz) * 0.425) -
      static_cast<int64_t>(measurementWindowHz / 2);
  if (usableHalfSpan <= 0) {
    return centers;
  }

  const int64_t preferredStep =
      static_cast<int64_t>(static_cast<double>(captureRateHz) * 0.60);
  const int64_t centerStep =
      std::max<int64_t>(gridStepHz, (preferredStep / gridStepHz) * gridStepHz);
  int64_t center = static_cast<int64_t>(startHz) + gridStepHz / 2;
  const int64_t stop = endHz;
  while (true) {
    centers.push_back(static_cast<uint32_t>(center));
    if (center + usableHalfSpan >= stop) {
      break;
    }
    center += centerStep;
  }
  return centers;
}

Recommendation recommendWindow(const std::vector<Measurement> &measurements) {
  Recommendation out;
  std::vector<double> clean;
  clean.reserve(measurements.size());
  for (const Measurement &measurement : measurements) {
    if (!std::isfinite(measurement.compensatedDbfs)) {
      continue;
    }
    if (measurement.clipRatio >= fm_tuner::kCalibrationClipRatio) {
      out.saturatedCount++;
      continue;
    }
    clean.push_back(measurement.compensatedDbfs);
  }
  if (clean.size() < 10) {
    return out;
  }

  std::sort(clean.begin(), clean.end());
  const auto percentile = [&](double fraction) {
    const size_t index = std::min(
        clean.size() - 1,
        static_cast<size_t>(static_cast<double>(clean.size()) * fraction));
    return clean[index];
  };
  out.minimumDbfs = clean.front();
  out.p05Dbfs = percentile(0.05);
  out.medianDbfs = percentile(0.50);
  out.p95Dbfs = percentile(0.95);
  out.maximumDbfs = clean.back();
  out.floorDbfs = static_cast<int>(std::round(out.p05Dbfs - 3.0));
  out.ceilDbfs = static_cast<int>(std::round(out.p95Dbfs + 3.0));
  out.ceilDbfs = std::max(out.ceilDbfs, out.floorDbfs + 1);
  out.valid = true;
  return out;
}

} // namespace fm_tuner::calibration
