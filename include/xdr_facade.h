#ifndef XDR_FACADE_H
#define XDR_FACADE_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include "xdr_server.h"

struct XdrCommandState {
  XdrCommandState(uint32_t frequencyHz, int customGain, int agcMode,
                  int bandwidthHz, int volumePercent, int deemphasisMode,
                  bool forceMono)
      : requestedFrequencyHz(frequencyHz), requestedCustomGain(customGain),
        requestedAGCMode(agcMode), requestedBandwidthHz(bandwidthHz),
        requestedVolume(volumePercent), requestedDeemphasis(deemphasisMode),
        requestedForceMono(forceMono), pendingFrequency(false),
        pendingGain(false), pendingAGC(false), pendingBandwidth(false) {}

  std::atomic<uint32_t> requestedFrequencyHz;
  std::atomic<int> requestedCustomGain;
  std::atomic<int> requestedAGCMode;
  std::atomic<int> requestedBandwidthHz;
  std::atomic<int> requestedVolume;
  std::atomic<int> requestedDeemphasis;
  std::atomic<bool> requestedForceMono;
  std::atomic<bool> pendingFrequency;
  std::atomic<bool> pendingGain;
  std::atomic<bool> pendingAGC;
  std::atomic<bool> pendingBandwidth;
};

class XdrFacade {
public:
  struct Options {
    bool verboseLogging = false;
    bool useSdrppGainStrategy = false;
    bool allowClientGainOverride = true;
  };

  XdrFacade(XDRServer &server, XdrCommandState &state, Options options)
      : m_server(server), m_state(state), m_options(options) {}

  void configureServer(const std::string &password, bool guestMode);

  void installCallbacks(
      const std::function<void(int)> &setVolumePercent,
      const std::function<void()> &onStart, const std::function<void()> &onStop,
      const std::function<std::string(int)> &formatCustomGain);

private:
  XDRServer &m_server;
  XdrCommandState &m_state;
  Options m_options;
};

#endif
