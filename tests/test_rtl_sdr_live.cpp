#include "catch_compat.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "dsp_pipeline.h"
#include "rtl_sdr_device.h"
#include "signal_level.h"

namespace {

bool envEnabled(const char* name) {
  const char* value = std::getenv(name);
  if (!value) {
    return false;
  }
  return std::string(value) == "1" || std::string(value) == "true" ||
         std::string(value) == "TRUE" || std::string(value) == "yes";
}

bool envPresent(const char* name) {
  const char* value = std::getenv(name);
  return value && *value != '\0';
}

uint32_t envU32(const char* name, uint32_t fallback) {
  const char* value = std::getenv(name);
  if (!value || *value == '\0') {
    return fallback;
  }
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(value, &end, 10);
  if (!end || *end != '\0') {
    return fallback;
  }
  return static_cast<uint32_t>(parsed);
}

int envInt(const char* name, int fallback) {
  const char* value = std::getenv(name);
  if (!value || *value == '\0') {
    return fallback;
  }
  char* end = nullptr;
  const long parsed = std::strtol(value, &end, 10);
  if (!end || *end != '\0') {
    return fallback;
  }
  return static_cast<int>(parsed);
}

bool hasVariation(const std::vector<uint8_t>& iq) {
  if (iq.empty()) {
    return false;
  }
  const auto [minIt, maxIt] = std::minmax_element(iq.begin(), iq.end());
  return *minIt != *maxIt;
}

bool readIqWithRetries(RTLSDRDevice& dev, std::vector<uint8_t>& iq,
                       size_t requestedSamples, int attempts,
                       std::chrono::milliseconds sleepBetween, size_t& samplesOut) {
  samplesOut = 0;
  for (int attempt = 0; attempt < attempts && samplesOut == 0; ++attempt) {
    samplesOut = dev.readIQ(iq.data(), requestedSamples);
    if (samplesOut == 0) {
      std::this_thread::sleep_for(sleepBetween);
    }
  }
  return samplesOut > 0;
}

SignalLevelResult measureFrequencyLevel(RTLSDRDevice& dev, uint32_t freqKHz,
                                        uint32_t sampleRateHz,
                                        int appliedGainDb,
                                        int channelBandwidthHz = 56000) {
  REQUIRE(dev.setFrequency(freqKHz * 1000U));
  std::this_thread::sleep_for(std::chrono::milliseconds(120));

  // Burn 4 full ring's worth of samples to ensure we're past any residual
  // buffer from the previous tune frequency. Low-latency mode helps but is
  // not a hard guarantee against latent in-flight USB transfers.
  std::vector<uint8_t> iq(4096 * 2, 0);
  size_t discardedSamples = 0;
  for (int flush = 0; flush < 4; ++flush) {
    (void)readIqWithRetries(dev, iq, iq.size() / 2, 4,
                            std::chrono::milliseconds(15), discardedSamples);
  }

  double dbfsSum = 0.0;
  double compensatedSum = 0.0;
  double levelSum = 0.0;
  int validBlocks = 0;

  for (int block = 0; block < 3; ++block) {
    size_t samples = 0;
    REQUIRE(readIqWithRetries(dev, iq, iq.size() / 2, 8,
                              std::chrono::milliseconds(15), samples));
    REQUIRE(samples > 0);
    iq.resize(samples * 2);
    REQUIRE(hasVariation(iq));

    const SignalLevelResult signal = computeSignalLevel(
        iq.data(), samples, appliedGainDb, 0.5, 0.0, -80.0, -12.0, sampleRateHz,
        channelBandwidthHz);
    REQUIRE(std::isfinite(signal.dbfs));
    REQUIRE(std::isfinite(signal.compensatedDbfs));
    dbfsSum += signal.dbfs;
    compensatedSum += signal.compensatedDbfs;
    levelSum += signal.level120;
    validBlocks++;
    iq.assign(4096 * 2, 0);
  }

  SignalLevelResult average{};
  REQUIRE(validBlocks > 0);
  average.dbfs = dbfsSum / static_cast<double>(validBlocks);
  average.compensatedDbfs = compensatedSum / static_cast<double>(validBlocks);
  average.level120 = static_cast<float>(levelSum / static_cast<double>(validBlocks));
  return average;
}

struct LiveDspAnalysis {
  int bandwidthHz = 0;
  double rawDbfs = 0.0;
  double channelDbfs = 0.0;
  double audioRms = 0.0;
  double clipRatio = 0.0;
  double stereoLockRatio = 0.0;
  double averagePilot = 0.0;
  double averageBlend = 0.0;
  double averageQuality = 0.0;
};

double stereoAudioRms(const float* left, const float* right, size_t count) {
  if (!left || !right || count == 0) {
    return 0.0;
  }
  double sum = 0.0;
  for (size_t i = 0; i < count; ++i) {
    sum += static_cast<double>(left[i]) * left[i];
    sum += static_cast<double>(right[i]) * right[i];
  }
  return std::sqrt(sum / static_cast<double>(count * 2));
}

LiveDspAnalysis analyzeBandwidth(RTLSDRDevice& dev, uint32_t sampleRateHz,
                                 int appliedGainDb, int bandwidthHz,
                                 int deemphasisMode = 0) {
  constexpr int kInputRate = 256000;
  constexpr int kOutputRate = 32000;
  Config cfg{};
  cfg.processing.dsp_block_samples = 8192;
  cfg.processing.w0_bandwidth_hz = bandwidthHz;
  cfg.processing.stereo = true;

  REQUIRE(sampleRateHz >= static_cast<uint32_t>(kInputRate));
  REQUIRE((sampleRateHz % static_cast<uint32_t>(kInputRate)) == 0);
  const size_t iqDecimation = sampleRateHz / static_cast<uint32_t>(kInputRate);
  DspPipeline pipeline(kInputRate, kOutputRate, cfg.processing, false,
                       static_cast<size_t>(cfg.processing.dsp_block_samples),
                       iqDecimation);
  pipeline.setBandwidthHz(bandwidthHz);
  pipeline.setDeemphasisMode(deemphasisMode);
  pipeline.setForceMono(false);

  std::vector<uint8_t> iq(pipeline.sdrBlockSamples() * 2, 0);

  size_t discarded = 0;
  for (int i = 0; i < 2; ++i) {
    (void)readIqWithRetries(dev, iq, iq.size() / 2, 8, std::chrono::milliseconds(15),
                            discarded);
  }

  double rawDbfsSum = 0.0;
  double channelDbfsSum = 0.0;
  double clipRatioSum = 0.0;
  double stereoLockSum = 0.0;
  double pilotSum = 0.0;
  double blendSum = 0.0;
  double qualitySum = 0.0;
  double audioRmsSum = 0.0;
  int validBlocks = 0;

  for (int block = 0; block < 8; ++block) {
    size_t samples = 0;
    REQUIRE(readIqWithRetries(dev, iq, iq.size() / 2, 8,
                              std::chrono::milliseconds(15), samples));
    REQUIRE(samples == pipeline.sdrBlockSamples());
    REQUIRE(hasVariation(iq));

    const SignalLevelResult signal = computeSignalLevel(
        iq.data(), samples, appliedGainDb, 0.5, 0.0, -80.0, -12.0, sampleRateHz,
        bandwidthHz);
    DspPipeline::Result dspOut;
    REQUIRE(pipeline.process(
        iq.data(), samples, [&](const float*, size_t) {}, dspOut));
    REQUIRE(dspOut.outSamples > 0);

    rawDbfsSum += signal.dbfs;
    channelDbfsSum += std::isfinite(dspOut.channelPowerDbfs) ? dspOut.channelPowerDbfs
                                                             : signal.dbfs;
    clipRatioSum += std::max(signal.hardClipRatio, signal.nearClipRatio);
    stereoLockSum += dspOut.stereoDetected ? 1.0 : 0.0;
    pilotSum += static_cast<double>(dspOut.pilotTenthsKHz);
    blendSum += static_cast<double>(dspOut.stereoBlend);
    qualitySum += static_cast<double>(dspOut.stereoQuality);
    audioRmsSum += stereoAudioRms(dspOut.left, dspOut.right, dspOut.outSamples);
    validBlocks++;
  }

  REQUIRE(validBlocks > 0);
  LiveDspAnalysis out{};
  out.bandwidthHz = bandwidthHz;
  out.rawDbfs = rawDbfsSum / validBlocks;
  out.channelDbfs = channelDbfsSum / validBlocks;
  out.audioRms = audioRmsSum / validBlocks;
  out.clipRatio = clipRatioSum / validBlocks;
  out.stereoLockRatio = stereoLockSum / validBlocks;
  out.averagePilot = pilotSum / validBlocks;
  out.averageBlend = blendSum / validBlocks;
  out.averageQuality = qualitySum / validBlocks;
  return out;
}

} // namespace

TEST_CASE("RTLSDRDevice live smoke test with attached hardware", "[rtl_sdr_live]") {
  if (!envEnabled("FM_TUNER_RUN_RTL_SDR_LIVE")) {
    INFO("set FM_TUNER_RUN_RTL_SDR_LIVE=1 to run the real RTL-SDR smoke test");
    SUCCEED("RTL-SDR live smoke test not enabled");
    return;
  }

  const uint32_t deviceIndex = envU32("FM_TUNER_RTL_DEVICE_INDEX", 0);
  const uint32_t sampleRateHz = envU32("FM_TUNER_RTL_SAMPLE_RATE", 256000);
  const uint32_t freqKHz = envU32("FM_TUNER_RTL_FREQ_KHZ", 101100);
  const int ppm = envInt("FM_TUNER_RTL_PPM", 0);
  const bool hasExplicitPpm = envPresent("FM_TUNER_RTL_PPM");
  const int gainTenthsDb = envInt("FM_TUNER_RTL_GAIN_TENTHS_DB", -1);

  RTLSDRDevice dev(deviceIndex);
  REQUIRE(dev.connect());

  REQUIRE(dev.setSampleRate(sampleRateHz));
  REQUIRE(dev.setFrequency(freqKHz * 1000U));
  if (hasExplicitPpm) {
    REQUIRE(dev.setFrequencyCorrection(ppm));
  }

  if (gainTenthsDb >= 0) {
    REQUIRE(dev.setGainMode(true));
    REQUIRE(dev.setGain(static_cast<uint32_t>(gainTenthsDb)));
  } else {
    REQUIRE(dev.setGainMode(false));
    REQUIRE(dev.setAGC(true));
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(80));

  std::vector<uint8_t> iq(4096 * 2, 0);
  size_t samples = 0;
  for (int attempt = 0; attempt < 12 && samples == 0; ++attempt) {
    samples = dev.readIQ(iq.data(), iq.size() / 2);
    if (samples == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }

  REQUIRE(samples > 0);
  iq.resize(samples * 2);
  REQUIRE(hasVariation(iq));

  const SignalLevelResult signal = computeSignalLevel(
      iq.data(), samples, (gainTenthsDb >= 0) ? (gainTenthsDb / 10) : 0, 0.5, 0.0,
      -80.0, -12.0, sampleRateHz, 56000);

  REQUIRE(std::isfinite(signal.dbfs));
  REQUIRE(std::isfinite(signal.compensatedDbfs));
  REQUIRE(signal.hardClipRatio >= 0.0);
  REQUIRE(signal.nearClipRatio >= signal.hardClipRatio);

  dev.setLowLatencyMode(true);
  size_t lowLatencySamples = 0;
  for (int attempt = 0; attempt < 8 && lowLatencySamples == 0; ++attempt) {
    lowLatencySamples = dev.readIQ(iq.data(), std::min<size_t>(samples, 2048));
    if (lowLatencySamples == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  REQUIRE(lowLatencySamples > 0);

  REQUIRE(dev.setFrequency((freqKHz + 100) * 1000U));
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  size_t retunedSamples = 0;
  for (int attempt = 0; attempt < 8 && retunedSamples == 0; ++attempt) {
    retunedSamples = dev.readIQ(iq.data(), std::min<size_t>(samples, 2048));
    if (retunedSamples == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  REQUIRE(retunedSamples > 0);
  dev.disconnect();
}

TEST_CASE("RTLSDRDevice live signal strength comparison", "[rtl_sdr_live]") {
  if (!envEnabled("FM_TUNER_RUN_RTL_SDR_COMPARE")) {
    INFO("set FM_TUNER_RUN_RTL_SDR_COMPARE=1 to run the strong-vs-weak live comparison");
    SUCCEED("RTL-SDR live comparison test not enabled");
    return;
  }

  const uint32_t deviceIndex = envU32("FM_TUNER_RTL_DEVICE_INDEX", 0);
  const uint32_t sampleRateHz = envU32("FM_TUNER_RTL_SAMPLE_RATE", 256000);
  const uint32_t strongFreqKHz = envU32("FM_TUNER_RTL_STRONG_FREQ_KHZ", 105900);
  const uint32_t weakFreqKHz = envU32("FM_TUNER_RTL_WEAK_FREQ_KHZ", 88200);
  const int manualGainTenthsDb =
      envInt("FM_TUNER_RTL_COMPARE_GAIN_TENTHS_DB", 180);

  RTLSDRDevice dev(deviceIndex);
  REQUIRE(dev.connect());
  REQUIRE(dev.setSampleRate(sampleRateHz));
  REQUIRE(dev.setGainMode(true));
  REQUIRE(dev.setGain(static_cast<uint32_t>(manualGainTenthsDb)));

  const SignalLevelResult strong =
      measureFrequencyLevel(dev, strongFreqKHz, sampleRateHz,
                            manualGainTenthsDb / 10);
  const SignalLevelResult weak =
      measureFrequencyLevel(dev, weakFreqKHz, sampleRateHz,
                            manualGainTenthsDb / 10);

  INFO("strong dbfs=" << strong.dbfs << " level=" << strong.level120
                      << " weak dbfs=" << weak.dbfs
                      << " level=" << weak.level120);
  REQUIRE(strong.compensatedDbfs > weak.compensatedDbfs);
  REQUIRE(strong.level120 > weak.level120 + 3.0f);

  dev.disconnect();
}

TEST_CASE("RTLSDRDevice live DSP analysis sweep", "[rtl_sdr_live]") {
  if (!envEnabled("FM_TUNER_RUN_RTL_SDR_ANALYZE")) {
    INFO("set FM_TUNER_RUN_RTL_SDR_ANALYZE=1 to run live DSP analysis");
    SUCCEED("RTL-SDR live DSP analysis not enabled");
    return;
  }

  const uint32_t deviceIndex = envU32("FM_TUNER_RTL_DEVICE_INDEX", 0);
  const uint32_t sampleRateHz = envU32("FM_TUNER_RTL_SAMPLE_RATE", 256000);
  const uint32_t freqKHz = envU32("FM_TUNER_ANALYZE_FREQ_KHZ", 88600);
  const int gainTenthsDb = envInt("FM_TUNER_RTL_GAIN_TENTHS_DB", -1);
  const std::array<int, 4> bandwidths = {194000, 168000, 151000, 133000};

  RTLSDRDevice dev(deviceIndex);
  REQUIRE(dev.connect());
  REQUIRE(dev.setSampleRate(sampleRateHz));
  REQUIRE(dev.setFrequency(freqKHz * 1000U));
  if (gainTenthsDb >= 0) {
    REQUIRE(dev.setGainMode(true));
    REQUIRE(dev.setGain(static_cast<uint32_t>(gainTenthsDb)));
  } else {
    REQUIRE(dev.setGainMode(false));
    REQUIRE(dev.setAGC(true));
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(120));

  const int appliedGainDb = (gainTenthsDb >= 0) ? (gainTenthsDb / 10) : 0;
  std::vector<LiveDspAnalysis> results;
  results.reserve(bandwidths.size());
  for (const int bandwidthHz : bandwidths) {
    results.push_back(analyzeBandwidth(dev, sampleRateHz, appliedGainDb, bandwidthHz));
  }

  std::cout << "\n[ANALYZE] freq_khz=" << freqKHz
            << " sample_rate=" << sampleRateHz
            << " gain_tenths_db=" << gainTenthsDb << "\n";
  std::cout << std::fixed << std::setprecision(2);
  for (const LiveDspAnalysis& row : results) {
    std::cout << "[ANALYZE] bw=" << row.bandwidthHz
              << " raw_dbfs=" << row.rawDbfs
              << " chan_dbfs=" << row.channelDbfs
              << " audio_rms=" << row.audioRms
              << " clip=" << row.clipRatio
              << " stereo_lock=" << row.stereoLockRatio
              << " pilot=" << row.averagePilot
              << " quality=" << row.averageQuality
              << " blend=" << row.averageBlend << "\n";
  }

  for (const LiveDspAnalysis& row : results) {
    REQUIRE(std::isfinite(row.rawDbfs));
    REQUIRE(std::isfinite(row.channelDbfs));
    REQUIRE(std::isfinite(row.audioRms));
    REQUIRE(row.clipRatio >= 0.0);
    REQUIRE(row.stereoLockRatio >= 0.0);
    REQUIRE(row.stereoLockRatio <= 1.0);
  }

  dev.disconnect();
}

TEST_CASE("RTLSDRDevice FM band sweep for calibration", "[rtl_sdr_live]") {
  if (!envEnabled("FM_TUNER_RUN_RTL_SDR_BANDSWEEP")) {
    INFO("set FM_TUNER_RUN_RTL_SDR_BANDSWEEP=1 to run the full-band calibration sweep");
    SUCCEED("RTL-SDR band sweep test not enabled");
    return;
  }

  const uint32_t deviceIndex = envU32("FM_TUNER_RTL_DEVICE_INDEX", 0);
  const uint32_t sampleRateHz = envU32("FM_TUNER_RTL_SAMPLE_RATE", 256000);
  const uint32_t startKHz = envU32("FM_TUNER_BANDSWEEP_START_KHZ", 87500);
  const uint32_t endKHz = envU32("FM_TUNER_BANDSWEEP_END_KHZ", 108000);
  const uint32_t stepKHz = envU32("FM_TUNER_BANDSWEEP_STEP_KHZ", 100);
  const int gainTenthsDb = envInt("FM_TUNER_RTL_GAIN_TENTHS_DB", 197);
  const bool autoGain = envInt("FM_TUNER_BANDSWEEP_AUTOGAIN", 0) != 0;

  RTLSDRDevice dev(deviceIndex);
  REQUIRE(dev.connect());
  REQUIRE(dev.setSampleRate(sampleRateHz));
  // Drop ring backlog on retune. Without this, the FIFO contains samples
  // from previous tune frequencies and the per-frequency measurement smears
  // across recent neighbors.
  dev.setLowLatencyMode(true);
  if (autoGain) {
    REQUIRE(dev.setGainMode(false));
  } else {
    REQUIRE(dev.setGainMode(true));
    REQUIRE(dev.setGain(static_cast<uint32_t>(gainTenthsDb)));
  }

  struct Row {
    uint32_t freqKHz;
    double dbfs;
    double compensatedDbfs;
    double level120;
  };
  std::vector<Row> rows;
  rows.reserve(((endKHz - startKHz) / stepKHz) + 1);

  // FM broadcast occupies ~150 kHz of channel bandwidth and the running app's
  // [SIG] line measures with `processing.w0_bandwidth_hz` (194 kHz default).
  // Using 56 kHz here would concentrate the measurement on the central DC
  // region where the R820T's LO-leakage notch suppresses signal — producing
  // a deceptively low reading at the actual station frequency and a falsely
  // strong reading 200 kHz off where the modulation lands outside the notch.
  const int sweepChannelBwHz = static_cast<int>(
      envU32("FM_TUNER_BANDSWEEP_CHANNEL_BW_HZ", 194000));
  for (uint32_t f = startKHz; f <= endKHz; f += stepKHz) {
    const SignalLevelResult r =
        measureFrequencyLevel(dev, f, sampleRateHz,
                              autoGain ? 0 : gainTenthsDb / 10,
                              sweepChannelBwHz);
    rows.push_back({f, r.dbfs, r.compensatedDbfs, r.level120});
  }

  // Compute robust min / max / percentiles on raw dBFS.
  std::vector<double> sortedDbfs;
  sortedDbfs.reserve(rows.size());
  for (const Row& row : rows) {
    sortedDbfs.push_back(row.dbfs);
  }
  std::sort(sortedDbfs.begin(), sortedDbfs.end());
  const double p05 = sortedDbfs[static_cast<size_t>(sortedDbfs.size() * 0.05)];
  const double p50 = sortedDbfs[sortedDbfs.size() / 2];
  const double p95 = sortedDbfs[static_cast<size_t>(sortedDbfs.size() * 0.95)];
  const double pMax = sortedDbfs.back();
  const double pMin = sortedDbfs.front();

  std::cout << "[SWEEP] freq_khz dbfs compensated_dbfs level120\n";
  for (const Row& row : rows) {
    std::cout << "[SWEEP] " << row.freqKHz << " " << row.dbfs << " "
              << row.compensatedDbfs << " " << row.level120 << "\n";
  }
  std::cout << "[SWEEP-SUMMARY] count=" << rows.size()
            << " min=" << pMin << " p05=" << p05 << " p50=" << p50
            << " p95=" << p95 << " max=" << pMax << "\n";
  std::cout << "[SWEEP-RECOMMENDATION] signal_floor_dbfs="
            << std::round(p05 - 3.0) << " signal_ceil_dbfs="
            << std::round(p95 + 3.0) << " range="
            << (std::round(p95 + 3.0) - std::round(p05 - 3.0)) << " dB\n";

  dev.disconnect();
}
