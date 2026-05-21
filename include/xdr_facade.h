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
        requestedForceMono(forceMono), requestedBlendMode(-1),
        pendingFrequency(false), pendingGain(false), pendingAGC(false),
        pendingBandwidth(false), pendingBlendMode(false) {}

  std::atomic<uint32_t> requestedFrequencyHz;
  std::atomic<int> requestedCustomGain;
  std::atomic<int> requestedAGCMode;
  std::atomic<int> requestedBandwidthHz;
  std::atomic<int> requestedVolume;
  std::atomic<int> requestedDeemphasis;
  std::atomic<bool> requestedForceMono;
  // -1 = no live request (use startup INI/CLI value); 0=soft, 1=normal,
  // 2=aggressive when set by an XDR 'Fb<n>' command.
  std::atomic<int> requestedBlendMode;
  std::atomic<bool> pendingFrequency;
  std::atomic<bool> pendingGain;
  std::atomic<bool> pendingAGC;
  std::atomic<bool> pendingBandwidth;
  std::atomic<bool> pendingBlendMode;
};

class XdrFacade {
public:
  struct Options {
    bool verboseLogging = false;
    bool useSdrppGainStrategy = false;
    bool allowClientGainOverride = true;
    bool fixedLocalGain = false;
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
