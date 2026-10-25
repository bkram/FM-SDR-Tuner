#include "scan_engine.h"

#include "tuning_limits.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <thread>
#include <vector>

#include "dsp/liquid_primitives.h"

#include "signal_level.h"

namespace {

// A local maximum of the fine curve is only reported as a station when it
// stands at least this far above the highest saddle that connects it to a
// higher peak. The noise maxima of an empty band and the shoulders of a
// station's own hump never clear this; a real station towers over everything
// around it.
constexpr double kStationProminenceDb = 8.0;

// Two peaks closer than this belong to the same station: the peak of a
// station and the ripple of its own skirt are a few kHz apart, while two
// stations are at least 200 kHz apart on the CCIR raster.
constexpr int64_t kStationMergeRadiusHz = 70000;

// The fine measurement grid never steps further than this, so that a station
// always falls inside at least one measurement window.
constexpr int64_t kTargetFineStepHz = 5000;

} // namespace

ScanEngine::ScanEngine()
    : m_active(false), m_restoreFreqHz(0), m_restoreBandwidthHz(0) {}

void ScanEngine::handleControl(
    XDRServer &xdrServer, uint32_t currentFreqHz, int currentBandwidthHz,
    bool rtlConnected, bool verboseLogging, std::atomic<int> &requestedBandwidthHz,
    std::atomic<bool> &pendingBandwidth,
    const std::function<void(uint32_t, int)> &restoreAfterScan) {
  XDRServer::ScanConfig newScanConfig;
  if (xdrServer.consumeScanStart(newScanConfig)) {
    m_config = newScanConfig;
    m_active = true;
    m_restoreFreqHz = currentFreqHz;
    m_restoreBandwidthHz = currentBandwidthHz;
    if (m_config.bandwidthHz > 0) {
      requestedBandwidthHz = m_config.bandwidthHz;
      pendingBandwidth = true;
    }
    if (verboseLogging) {
      std::cout << "[SCAN] start "
                << "from=" << m_config.startKHz << " to=" << m_config.stopKHz
                << " step=" << m_config.stepKHz << " bw=" << m_config.bandwidthHz
                << " mode=" << (m_config.continuous ? "continuous" : "single")
                << "\n";
    }
  }

  if (xdrServer.consumeScanCancel()) {
    const bool wasActive = m_active;
    m_active = false;
    if (verboseLogging) {
      std::cout << "[SCAN] cancel requested\n";
    }
    if (wasActive && rtlConnected) {
      restoreAfterScan(m_restoreFreqHz, m_restoreBandwidthHz);
    }
  }
}

bool ScanEngine::runIfActive(
    XDRServer &xdrServer, bool rtlConnected, const std::function<bool()> &shouldRun,
    const std::function<bool(uint32_t)> &tunerSetFrequency,
    const std::function<size_t(uint8_t *, size_t)> &tunerReadIQ,
    const std::function<void(const uint8_t *, size_t)> &writeIqCapture,
    const std::chrono::milliseconds &scanRetrySleep, uint8_t *iqBuffer,
    size_t sdrBufSamples, uint32_t iqSampleRate, int effectiveAppliedGainDb,
    double signalGainCompFactor,
    const Config::SDRSection &sdrConfig,
    const std::function<void(uint32_t, int)> &restoreAfterScan,
    const std::function<void()> &tunerFlush) {
  if (!m_active || !rtlConnected) {
    return false;
  }

  constexpr int kScanRetries = 1;
  constexpr int kFftAverages = 1;
  constexpr size_t kScanReadSamplesCap = 32768;
  // After a scan retune the source's buffered IQ is still from the previous
  // center. flushBuffers() drops that backlog; we then discard a settle window
  // to skip the PLL-lock + USB-in-flight samples captured before the new
  // center took effect, so the FFT only sees post-retune spectrum. Without
  // this the strong-station energy lands one sweep-step too high.
  const size_t kRetuneSettleSamples =
      std::max<size_t>(8192, iqSampleRate / 10); // ~100 ms
  constexpr float kUsableSpectrumFraction = 0.85f;
  constexpr float kCenterStepFraction = 0.60f;

  const int startKHz = std::clamp(std::min(m_config.startKHz, m_config.stopKHz),
                                  static_cast<int>(fm_tuner::kFmBroadcastMinFreqKHz),
                                  static_cast<int>(fm_tuner::kFmBroadcastMaxFreqKHz));
  const int stopKHz = std::clamp(std::max(m_config.startKHz, m_config.stopKHz),
                                 static_cast<int>(fm_tuner::kFmBroadcastMinFreqKHz),
                                 static_cast<int>(fm_tuner::kFmBroadcastMaxFreqKHz));
  const int requestedStepKHz = std::max(5, m_config.stepKHz);
  const int channelBandwidthHz =
      std::clamp((m_config.bandwidthHz > 0) ? m_config.bandwidthHz : 56000,
                 10000, 200000);
  // The plot window the reference client draws the scan in.
  const bool scanUseClientWindow = sdrConfig.scan_use_client_window;
  const double scanClientMinLevel = sdrConfig.scan_client_min_level;
  const double scanClientMaxLevel = std::max(
      sdrConfig.scan_client_max_level, sdrConfig.scan_client_min_level + 1.0);
  // The whole spectrum travels in ONE line ("U<f>=<v>,<f>=<v>,..."). The
  // reference client reads into a 10000-byte buffer and silently drops
  // everything past it (research/xdr-gtk/src/tuner.c SERIAL_BUFFER), so a
  // line longer than that is truncated mid-entry: the trailing comma is
  // lost, the client's parser then discards the last entry, and the plot
  // ends short of the requested stop frequency. The client itself refuses
  // more than 700 samples (research/xdr-gtk/src/scan.c SCAN_MAX_SAMPLES),
  // so this is never the binding limit for a client that plays by the
  // rules — it only protects against a hand-rolled client asking for a
  // 5 kHz step over the whole band. Widen the step so the result fits.
  constexpr int kMaxScanChannels = 700;
  const int requestedChannelCount =
      ((stopKHz - startKHz) / requestedStepKHz) + 1;
  int stepKHz = requestedStepKHz;
  if (requestedChannelCount > kMaxScanChannels) {
    const int widenedStep =
        ((stopKHz - startKHz) / (kMaxScanChannels - 1)) + 1;
    stepKHz = std::max(stepKHz, widenedStep);
    std::cerr << "[SCAN] warning: " << requestedChannelCount
              << " channels requested exceeds the " << kMaxScanChannels
              << " the protocol carries in one line; widening the step from "
              << requestedStepKHz << " to " << stepKHz << " kHz\n";
  }
  // The client asks for one level per point of a (start, step) grid, but the
  // spectrum has to be measured at a resolution that never misses a station.
  // A station sitting between two grid points is half a step away from the
  // nearest one, so integrating a narrow channel window around the grid
  // points alone leaves most of the band blind: with a 200 kHz step a
  // station on 96.2 MHz is 100 kHz from the nearest grid point and is never
  // measured at all. The spectrum is therefore measured on a FINE grid, and
  // the station found on that fine grid is reported on the grid point it
  // belongs to.
  //
  // The fine step has to DIVIDE the reporting cell step. The captures of the
  // sweep are taken at different centre frequencies and the finest grid is
  // assembled from all of them, so the grid points of the different captures
  // have to fall on the same phase; otherwise the assembled curve mixes
  // several sampling phases and the steps between them look like stations of
  // their own (measured: a fine step that did not divide the cell step
  // turned the noise floor of an empty band into a picket fence of false
  // peaks, one of them as strong as a real station).
  int64_t gridStartHz = static_cast<int64_t>(startKHz) * 1000;
  int64_t gridStopHz = static_cast<int64_t>(stopKHz) * 1000;
  const int64_t cellStepHz = static_cast<int64_t>(stepKHz) * 1000;
  if (cellStepHz > 0) {
    // The European FM plan allocates channels every 200 kHz starting at
    // 87.6 MHz. A grid whose step divides 200 kHz and whose first point
    // sits on that raster contains every station exactly.
    constexpr int64_t kCcirChannelStepHz = 200000;
    constexpr int64_t kCcirRasterAnchorHz = 87600000;
    if ((kCcirChannelStepHz % cellStepHz) == 0) {
      // Shift the grid forward onto the raster's phase. The phase is taken
      // modulo the CELL step, not the raster step: what matters is that
      // every station frequency is congruent to the grid start.
      const int64_t phaseHz =
          ((gridStartHz - kCcirRasterAnchorHz) % cellStepHz + cellStepHz) %
          cellStepHz;
      if (phaseHz != 0) {
        gridStartHz += cellStepHz - phaseHz;
      }
    } else {
      std::cerr << "[SCAN] warning: a " << stepKHz
                << " kHz step does not divide the 200 kHz CCIR channel "
                   "raster, so some stations are reported at the nearest grid "
                   "point instead of their own frequency\n";
    }
    // The grid ends at the last point that is still inside the requested
    // range.
    gridStopHz = gridStartHz +
                 ((gridStopHz - gridStartHz) / cellStepHz) * cellStepHz;
  }
  const int channelCount =
      static_cast<int>((gridStopHz - gridStartHz) / cellStepHz) + 1;

  // The largest step that divides the cell step and still resolves a
  // station. The candidates are multiples of 500 Hz, which keeps the
  // measurement window aligned with the FFT bins of the capture.
  int64_t fineStepHz = cellStepHz;
  for (int64_t candidate = kTargetFineStepHz; candidate >= 1000;
       candidate -= 500) {
    if (cellStepHz % candidate == 0) {
      fineStepHz = candidate;
      break;
    }
  }
  if (cellStepHz % 1000 != 0) {
    fineStepHz = cellStepHz;
  }

  const int fineCount =
      static_cast<int>((gridStopHz - gridStartHz) / fineStepHz) + 1;
  // The band power measured at every point of the fine grid. A point that has
  // not been measured yet stays NaN: an unmeasured point must not look like
  // an empty part of the band.
  std::vector<double> bandPowerByFine(static_cast<size_t>(fineCount),
                                       std::numeric_limits<double>::quiet_NaN());
  // The reported level of every cell of the reporting grid. A cell without a
  // station reports 0.0: the plot shows the stations and nothing else, so a
  // frequency nobody broadcasts on cannot show up as a signal.
  std::vector<float> levelByChannel(static_cast<size_t>(channelCount), 0.0f);

  if (iqSampleRate == 0) {
    if (!m_config.continuous || !m_active) {
      m_active = false;
      restoreAfterScan(m_restoreFreqHz, m_restoreBandwidthHz);
    }
    return true;
  }

  const int64_t sampleRateHz = static_cast<int64_t>(iqSampleRate);
  const int64_t windowHalfHz = fm_tuner::kSignalChannelWindowHz / 2;
  // Usable complex spectrum is Fs wide in total (±Fs/2). The half-span (from
  // tuned center to the edge of the usable window) is therefore
  // (Fs * kUsableSpectrumFraction) / 2 — but we also need to leave room for
  // the measurement window (±halfChannelBW) so it does not wrap across
  // Nyquist and alias content from the opposite side of the band.
  const int64_t maxHalfSpanByFraction =
      static_cast<int64_t>((static_cast<double>(sampleRateHz) *
                            kUsableSpectrumFraction) /
                           2.0);
  const int64_t maxHalfSpanByNyquist =
      std::max<int64_t>(1, (sampleRateHz / 2) - windowHalfHz);
  const int64_t usableHalfSpanHz =
      std::min(maxHalfSpanByFraction, maxHalfSpanByNyquist);
  // Keep consecutive captures overlapping by at least ~25 %. This matters
  // when a wide scan bandwidth forces usableHalfSpanHz small enough that the
  // preferred Fs-fraction step would leave gaps between captures and push
  // every in-gap channel into the per-channel fallback path.
  const int64_t coverageCapHz =
      std::max<int64_t>(1, (usableHalfSpanHz * 3) / 2);
  const int64_t preferredStepHz =
      static_cast<int64_t>(static_cast<double>(sampleRateHz) *
                           kCenterStepFraction);
  // The capture centres must not land on a reporting grid point. The
  // stations live on the grid, and a capture centred on a station measures
  // the station with the tuner's own centre in the middle of its window.
  //
  // The centres therefore run half a cell away from every grid point, and
  // their step is a whole number of cells so the phase never drifts back
  // onto a grid point. The step is rounded DOWN to a whole number of cells
  // so the captures keep the overlap they had before; rounding up would
  // leave consecutive captures touching exactly, with the stations on the
  // seam.
  const int64_t baseStepHz = std::min(preferredStepHz, coverageCapHz);
  const int64_t centerStepHz = std::max<int64_t>(
      cellStepHz, (baseStepHz / cellStepHz) * cellStepHz);
  // The first centre of the half-cell phase class whose span still covers
  // the start of the grid.
  int64_t firstCenterHz = gridStartHz + (cellStepHz / 2);
  while (firstCenterHz - usableHalfSpanHz > gridStartHz) {
    firstCenterHz -= cellStepHz;
  }
  // The last centre whose span still reaches the end of the grid.
  int64_t endCenterHz = firstCenterHz;
  while (endCenterHz + centerStepHz + usableHalfSpanHz <= gridStopHz) {
    endCenterHz += centerStepHz;
  }
  const size_t scanReadSamples =
      std::min(sdrBufSamples, std::max<size_t>(8192, kScanReadSamplesCap));

  // The scan reads the spectrum through the same estimator the signal meter
  // uses, so a station shows the same number in the plot as on the meter.
  thread_local IqSpectrum spectrum;

  // Measure the band power around every fine point this capture covers.
  auto measureCapture = [&](int64_t tunedCenterHz, size_t samples,
                            int firstFine, int lastFine) -> bool {
    if (!spectrum.analyse(iqBuffer, samples, iqSampleRate)) {
      return false;
    }
    const int windowHz = fm_tuner::kSignalChannelWindowHz;
    for (int n = firstFine; n <= lastFine; n++) {
      const int64_t fHz = gridStartHz + static_cast<int64_t>(n) * fineStepHz;
      const int64_t offsetHz = fHz - tunedCenterHz;
      // A window that reaches past the analysed spectrum would read only
      // part of the band and report a level that is too low.
      if (std::llabs(offsetHz) + windowHalfHz > spectrum.halfSpanHz()) {
        continue;
      }
      const double power = spectrum.channelPower(offsetHz, windowHz);
      if (power <= 0.0) {
        continue;
      }
      double &slot = bandPowerByFine[static_cast<size_t>(n)];
      // The captures overlap, and the tuner's analogue response ripples by
      // tens of dB across the band, so the strongest measurement of a point
      // is the one that saw it through the best part of the response.
      if (!std::isfinite(slot) || power > slot) {
        slot = power;
      }
    }
    return true;
  };

  // Flush the stale pre-retune backlog, then read (and discard) a settle
  // window of fresh samples so the next measured read is clean.
  auto settleAfterRetune = [&]() {
    if (tunerFlush) {
      tunerFlush();
    }
    size_t discarded = 0;
    int emptyReads = 0;
    while (discarded < kRetuneSettleSamples && emptyReads < 4) {
      const size_t want =
          std::min(sdrBufSamples, kRetuneSettleSamples - discarded);
      const size_t got = tunerReadIQ(iqBuffer, want);
      if (got == 0) {
        emptyReads++;
        std::this_thread::sleep_for(scanRetrySleep);
        continue;
      }
      emptyReads = 0;
      discarded += got;
    }
  };

  // Report the band on the reporting grid and find the stations on it.
  //
  // Every grid point reports the level of its own measurement, so the client
  // draws the spectrum of the band instead of a row of spikes. That is what
  // keeps a signal that is not a station where it belongs: it stays a bump in
  // the noise floor, the way a spectrum display shows it, instead of turning
  // into a station of its own. A grid point that holds a station reports the
  // station's peak instead, so the station sits exactly on its own grid
  // point and shows its real strength.
  //
  // A station is a local maximum of the band-power curve that stands at
  // least kStationProminenceDb above the highest saddle that connects it to
  // a higher peak. That is what separates a station from the noise maxima of
  // an empty band and from the shoulders of a station's own hump: neither
  // rises more than a few dB above the saddle that joins them to the
  // station itself, while a real station towers over its surroundings.
  auto detectStations = [&]() {
    std::fill(levelByChannel.begin(), levelByChannel.end(), 0.0f);

    // The level of a grid point is the level the meter shows for a tuner
    // parked on it: the same spectrum, the same estimator, the same window.
    const int64_t cellToFine =
        (fineStepHz > 0 && cellStepHz % fineStepHz == 0) ? cellStepHz / fineStepHz
                                                          : 1;
    for (int ch = 0; ch < channelCount; ch++) {
      const int64_t fine = static_cast<int64_t>(ch) * cellToFine;
      if (fine < 0 ||
          fine >= static_cast<int64_t>(bandPowerByFine.size())) {
        continue;
      }
      const double power = bandPowerByFine[static_cast<size_t>(fine)];
      if (!std::isfinite(power) || power <= 0.0) {
        continue;
      }
      levelByChannel[static_cast<size_t>(ch)] = computeDisplaySignalLevel120(
          10.0 * std::log10(power), 0.0, effectiveAppliedGainDb,
          signalGainCompFactor, sdrConfig.signal_bias_db,
          sdrConfig.signal_floor_dbfs, sdrConfig.signal_ceil_dbfs, false);
    }

    const size_t points = bandPowerByFine.size();
    std::vector<double> curveDb(points, -400.0);
    std::vector<char> measured(points, 0);
    for (size_t i = 0; i < points; i++) {
      const double power = bandPowerByFine[i];
      if (std::isfinite(power) && power > 0.0) {
        curveDb[i] = 10.0 * std::log10(power);
        measured[i] = 1;
      }
    }

    struct Peak {
      double freqHz = 0.0;
      double levelDbfs = -200.0;
      double prominenceDb = 0.0;
    };
    std::vector<Peak> candidates;
    for (size_t i = 1; i + 1 < points; i++) {
      if (!measured[i] || !measured[i - 1] || !measured[i + 1]) {
        continue;
      }
      if (!(curveDb[i] > curveDb[i - 1] && curveDb[i] >= curveDb[i + 1])) {
        continue;
      }
      // The prominence of the peak: how far it rises above the highest
      // saddle that connects it to a higher peak on either side.
      const double height = curveDb[i];
      double saddleLeft = height;
      for (size_t j = i; j-- > 0;) {
        if (!measured[j] || curveDb[j] > height) {
          break;
        }
        saddleLeft = std::min(saddleLeft, curveDb[j]);
      }
      double saddleRight = height;
      for (size_t j = i + 1; j < points; j++) {
        if (!measured[j] || curveDb[j] > height) {
          break;
        }
        saddleRight = std::min(saddleRight, curveDb[j]);
      }
      const double prominence = height - std::max(saddleLeft, saddleRight);
      if (prominence < kStationProminenceDb) {
        continue;
      }
      // The peak sits between two fine points; interpolate its position.
      const double denominator = curveDb[i - 1] - 2.0 * curveDb[i] + curveDb[i + 1];
      double delta = 0.0;
      if (denominator != 0.0) {
        delta = 0.5 * (curveDb[i - 1] - curveDb[i + 1]) / denominator;
      }
      delta = std::clamp(delta, -1.0, 1.0);
      Peak peak;
      peak.freqHz = static_cast<double>(gridStartHz) +
                    static_cast<double>(i) * static_cast<double>(fineStepHz) +
                    delta * static_cast<double>(fineStepHz);
      peak.levelDbfs = height;
      peak.prominenceDb = prominence;
      candidates.push_back(peak);
    }

    // The strongest peak of a cluster is the station; the others are the
    // ripples of its own skirt.
    std::sort(candidates.begin(), candidates.end(),
              [](const Peak &a, const Peak &b) {
                return a.levelDbfs > b.levelDbfs;
              });
    std::vector<Peak> stations;
    for (const Peak &peak : candidates) {
      bool duplicate = false;
      for (const Peak &kept : stations) {
        if (std::llabs(static_cast<int64_t>(peak.freqHz) -
                       static_cast<int64_t>(kept.freqHz)) <
            kStationMergeRadiusHz) {
          duplicate = true;
          break;
        }
      }
      if (!duplicate) {
        stations.push_back(peak);
      }
    }

    for (const Peak &station : stations) {
      int64_t cell = (static_cast<int64_t>(std::llround(station.freqHz)) -
                      gridStartHz + cellStepHz / 2) /
                     cellStepHz;
      cell = std::clamp<int64_t>(cell, 0, channelCount - 1);
      // The level of a station is the same absolute number the meter shows
      // for it: the scan and the meter read the same spectrum through the
      // same estimator and integrate the same window.
      const float level = computeDisplaySignalLevel120(
          station.levelDbfs, 0.0, effectiveAppliedGainDb, signalGainCompFactor,
          sdrConfig.signal_bias_db, sdrConfig.signal_floor_dbfs,
          sdrConfig.signal_ceil_dbfs, false);
      float &slot = levelByChannel[static_cast<size_t>(cell)];
      if (level > slot) {
        slot = level;
      }
    }
  };

  // Build the "U" line for the channels measured so far and hand it to the
  // client. The xdr-gtk client REPLACES its dataset on every "U" line
  // (research/xdr-gtk/src/scan.c scan_update()) and never merges, so a line
  // must always describe the same grid: the same start frequency, the same
  // step, and the same channel count. The unmeasured channels are emitted as
  // 0.0 (the client's "no signal" floor) so the line keeps the full grid
  // width from the first frame on.
  //
  // The trailing comma is mandatory: the client's parser sizes its array
  // from the comma count and drops the last entry when the comma is
  // missing (research/xdr-gtk/src/tuner-scan.c tuner_scan_parse()).
  //
  // The measured levels are remapped from the meter's 0..120 onto the plot
  // window the client draws in (3..84 by default) when the client draws the
  // scan in a fixed window; with the client's relative scale the raw 0..120
  // is sent, which is the same number the meter shows.
  auto pushPartialScanLine = [&]() {
    detectStations();
    std::ostringstream scanLine;
    for (int ch = 0; ch < channelCount; ch++) {
      const float rfLevel = levelByChannel[static_cast<size_t>(ch)];
      float value = 0.0f;
      if (std::isfinite(rfLevel) && rfLevel > 0.0f) {
        value = scanUseClientWindow
                    ? static_cast<float>(
                          scanClientMinLevel +
                          (std::clamp(rfLevel, 0.0f, 120.0f) / 120.0f) *
                              (scanClientMaxLevel - scanClientMinLevel))
                    : rfLevel;
      }
      scanLine << ((gridStartHz + static_cast<int64_t>(ch) * cellStepHz) / 1000)
               << "=" << std::fixed << std::setprecision(1) << value << ",";
    }
    const std::string line = scanLine.str();
    if (!line.empty()) {
      xdrServer.pushScanLine(line);
    }
  };

  for (int64_t centerHz = firstCenterHz; centerHz <= endCenterHz;
       centerHz += centerStepHz) {
    if (!shouldRun() || xdrServer.consumeScanCancel()) {
      m_active = false;
      break;
    }

    if (!tunerSetFrequency(static_cast<uint32_t>(centerHz))) {
      std::cerr << "[SCAN] warning: failed to retune to "
                << (centerHz / 1000) << " kHz; aborting scan\n";
      m_active = false;
      break;
    }
    // Drop the stale pre-retune backlog and let the tuner/NCO settle.
    settleAfterRetune();

    for (int avg = 0; avg < kFftAverages; avg++) {
      size_t samples = 0;
      for (int retries = 0; retries < kScanRetries && samples == 0; retries++) {
        samples = tunerReadIQ(iqBuffer, scanReadSamples);
        if (samples == 0) {
          std::this_thread::sleep_for(scanRetrySleep);
        }
      }
      if (samples == 0) {
        continue;
      }

      writeIqCapture(iqBuffer, samples);

      (void)measureCapture(centerHz, samples, 0, fineCount - 1);
    }

    // Publish what has been measured so far. The client shows the plot
    // filling in instead of staring at an empty graph for the whole sweep
    // (a full 87.5-108 MHz sweep takes tens of seconds on an RTL dongle).
    pushPartialScanLine();
  }

  // Fallback for uncovered fine points so the client receives complete scan
  // lines. Batch contiguous uncovered points into the largest span one
  // retune can cover, instead of retuning once per missing point.
  //
  // A cancelled or aborted sweep skips the fallback: after an early cancel
  // nearly every fine point is uncovered, and filling them in would keep the
  // tuner off the listener's station for many seconds after the cancel.
  for (int n = 0; m_active && n < fineCount;) {
    if (std::isfinite(bandPowerByFine[static_cast<size_t>(n)])) {
      n++;
      continue;
    }
    if (!shouldRun() || xdrServer.consumeScanCancel()) {
      m_active = false;
      break;
    }

    const int batchStart = n;
    const int64_t batchStartHz = gridStartHz + static_cast<int64_t>(n) * fineStepHz;
    const int64_t batchMaxStopHz = batchStartHz + (usableHalfSpanHz * 2);
    int batchEnd = n;
    while ((batchEnd + 1) < fineCount &&
           !std::isfinite(bandPowerByFine[static_cast<size_t>(batchEnd + 1)])) {
      const int64_t nextHz =
          gridStartHz + static_cast<int64_t>(batchEnd + 1) * fineStepHz;
      if (nextHz > batchMaxStopHz) {
        break;
      }
      batchEnd++;
    }

    const int64_t batchEndHz =
        gridStartHz + static_cast<int64_t>(batchEnd) * fineStepHz;
    const uint32_t batchCenterHz =
        static_cast<uint32_t>((batchStartHz + batchEndHz) / 2);

    bool measured = false;
    if (tunerSetFrequency(batchCenterHz)) {
      settleAfterRetune();
      size_t samples = 0;
      for (int retries = 0; retries < kScanRetries && samples == 0; retries++) {
        samples =
            tunerReadIQ(iqBuffer, std::min(sdrBufSamples, static_cast<size_t>(4096)));
        if (samples == 0) {
          std::this_thread::sleep_for(scanRetrySleep);
        }
      }
      if (samples > 0) {
        writeIqCapture(iqBuffer, samples);
        measured = measureCapture(batchCenterHz, samples, batchStart, batchEnd);
      }
    } else {
      std::cerr << "[SCAN] warning: failed to retune to "
                << (batchCenterHz / 1000) << " kHz during fallback sampling\n";
    }

    if (!measured) {
      // Measure the remaining points one by one, each from its own tune.
      for (int missing = batchStart; missing <= batchEnd; missing++) {
        const int64_t fineHz =
            gridStartHz + static_cast<int64_t>(missing) * fineStepHz;
        if (!tunerSetFrequency(static_cast<uint32_t>(fineHz))) {
          std::cerr << "[SCAN] warning: failed to retune to " << (fineHz / 1000)
                    << " kHz during per-channel fallback sampling\n";
          continue;
        }
        settleAfterRetune();
        size_t singleSamples = 0;
        for (int retries = 0; retries < kScanRetries && singleSamples == 0;
             retries++) {
          singleSamples = tunerReadIQ(
              iqBuffer, std::min(sdrBufSamples, static_cast<size_t>(4096)));
          if (singleSamples == 0) {
            std::this_thread::sleep_for(scanRetrySleep);
          }
        }
        if (singleSamples == 0) {
          continue;
        }
        writeIqCapture(iqBuffer, singleSamples);
        (void)measureCapture(fineHz, singleSamples, missing, missing);
      }
    }

    n = batchEnd + 1;
  }

  // Final pass: the fallback sampling above has filled in the channels the
  // main sweep could not cover, so the complete grid is published once more.
  pushPartialScanLine();

  if (!m_config.continuous || !m_active) {
    m_active = false;
    restoreAfterScan(m_restoreFreqHz, m_restoreBandwidthHz);
  }
  return true;
}
