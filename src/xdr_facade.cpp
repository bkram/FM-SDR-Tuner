#include "xdr_facade.h"

#include <algorithm>
#include <iostream>

void XdrFacade::configureServer(const std::string &password, bool guestMode) {
  m_server.setVerboseLogging(m_options.verboseLogging);
  m_server.setFrequencyState(m_state.requestedFrequencyHz.load());
  if (!password.empty()) {
    m_server.setPassword(password);
  }
  if (guestMode) {
    m_server.setGuestMode(true);
  }
}

void XdrFacade::installCallbacks(
    const std::function<void(int)> &setVolumePercent,
    const std::function<void()> &onStart, const std::function<void()> &onStop,
    const std::function<std::string(int)> &formatCustomGain) {
  auto setVolumePercentCb = setVolumePercent;
  auto onStartCb = onStart;
  auto onStopCb = onStop;
  auto formatCustomGainCb = formatCustomGain;

  m_server.setFrequencyCallback([&](uint32_t freqHz) {
    if (m_options.verboseLogging) {
      std::cout << "[XDR] tuning to " << (freqHz / 1000) << " kHz\n";
    }
    m_state.requestedFrequencyHz.store(freqHz, std::memory_order_relaxed);
    m_state.pendingFrequency.store(true, std::memory_order_release);
  });

  m_server.setVolumeCallback([&, setVolumePercentCb](int volume) {
    m_state.requestedVolume = std::clamp(volume, 0, 100);
    setVolumePercentCb(m_state.requestedVolume.load());
  });

  m_server.setGainCallback([&, formatCustomGainCb](int newGain) -> bool {
    if (m_options.useSdrppGainStrategy) {
      if (m_options.verboseLogging) {
        std::cout << "[XDR] G command ignored (gain_strategy=sdrpp)\n";
      }
      return false;
    }
    if (!m_options.allowClientGainOverride) {
      if (m_options.verboseLogging) {
        std::cout << "[XDR] G command ignored (client_gain_allowed=false)\n";
      }
      return false;
    }
    const int rf = ((newGain / 10) % 10) ? 1 : 0;
    const int ifv = (newGain % 10) ? 1 : 0;
    m_state.requestedCustomGain = rf * 10 + ifv;
    if (m_options.verboseLogging) {
      std::cout << "[XDR] G" << formatCustomGainCb(newGain)
                << " received -> rf=" << rf << " if=" << ifv << "\n";
    }
    m_state.pendingGain = true;
    return true;
  });

  m_server.setAGCCallback([&](int agcMode) -> bool {
    if (m_options.useSdrppGainStrategy) {
      if (m_options.verboseLogging) {
        std::cout << "[XDR] A command ignored (gain_strategy=sdrpp)\n";
      }
      return false;
    }
    if (!m_options.allowClientGainOverride) {
      if (m_options.verboseLogging) {
        std::cout << "[XDR] A command ignored (client_gain_allowed=false)\n";
      }
      return false;
    }
    const int clipped = std::clamp(agcMode, 0, 3);
    m_state.requestedAGCMode = clipped;
    if (m_options.verboseLogging) {
      std::cout << "[XDR] A" << clipped << " received\n";
    }
    m_state.pendingAGC = true;
    return true;
  });

  m_server.setModeCallback([&](int mode) {
    if (m_options.verboseLogging && mode != 0) {
      std::cout << "[XDR] mode " << mode << " requested (FM demod path only)\n";
    }
  });

  m_server.setBandwidthCallback([&](int bandwidth) {
    m_state.requestedBandwidthHz = std::clamp(bandwidth, 0, 400000);
    m_state.pendingBandwidth = true;
    if (m_options.verboseLogging) {
      std::cout << "[XDR] W" << m_state.requestedBandwidthHz.load()
                << " received\n";
    }
  });

  m_server.setDeemphasisCallback([&](int deemphasis) {
    m_state.requestedDeemphasis = std::clamp(deemphasis, 0, 2);
  });

  m_server.setForceMonoCallback(
      [&](bool forceMono) { m_state.requestedForceMono = forceMono; });

  m_server.setStartCallback([&, onStartCb]() {
    if (m_options.verboseLogging) {
      std::cout << "[XDR] tuner started by client\n";
    }
    onStartCb();
  });

  m_server.setStopCallback([&, onStopCb]() {
    if (m_options.verboseLogging) {
      std::cout << "[XDR] tuner stopped by client\n";
    }
    onStopCb();
  });
}
