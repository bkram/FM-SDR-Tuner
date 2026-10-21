#include "catch_compat.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#define private public
#include "xdr_server.h"
#undef private

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketLen = int;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketLen = socklen_t;
#endif

namespace {

#if defined(_WIN32)
struct WinSockInit {
  WinSockInit() {
    WSADATA wsaData{};
    const int rc = WSAStartup(MAKEWORD(2, 2), &wsaData);
    REQUIRE(rc == 0);
  }
  ~WinSockInit() { WSACleanup(); }
};
#endif

void closeSock(int sock) {
#if defined(_WIN32)
  closesocket(static_cast<SOCKET>(sock));
#else
  close(sock);
#endif
}

uint16_t reserveLoopbackPort() {
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    return 0;
  }

  sockaddr_in addr{};
#if defined(__APPLE__)
  addr.sin_len = sizeof(addr);
#endif
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    closeSock(sock);
    return 0;
  }

  sockaddr_in actual{};
  SocketLen len = sizeof(actual);
  if (getsockname(sock, reinterpret_cast<sockaddr *>(&actual), &len) != 0) {
    closeSock(sock);
    return 0;
  }
  const uint16_t port = ntohs(actual.sin_port);
  closeSock(sock);
  return port;
}

int connectLoopback(uint16_t port) {
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE(sock >= 0);

  sockaddr_in addr{};
#if defined(__APPLE__)
  addr.sin_len = sizeof(addr);
#endif
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);

  const int rc = connect(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
  REQUIRE(rc == 0);
  return sock;
}

} // namespace

TEST_CASE("XDR processCommand updates state and invokes callbacks", "[xdr_unit]") {
  XDRServer xdr(7374);
  xdr.setVerboseLogging(false);

  std::atomic<uint32_t> tuned{0};
  std::atomic<int> volume{0};
  std::atomic<int> agc{0};
  std::atomic<int> sampleInterval{0};
  std::atomic<int> detector{0};
  std::atomic<bool> mono{false};
  std::atomic<int> startCalls{0};
  std::atomic<int> stopCalls{0};

  xdr.setFrequencyCallback([&](uint32_t f) { tuned = f; });
  xdr.setVolumeCallback([&](int v) { volume = v; });
  xdr.setAGCCallback([&](int a) {
    agc = a;
    return true;
  });
  xdr.setSamplingCallback([&](int i, int d) {
    sampleInterval = i;
    detector = d;
  });
  xdr.setForceMonoCallback([&](bool v) { mono = v; });
  xdr.setStartCallback([&]() { startCalls++; });
  xdr.setStopCallback([&]() { stopCalls++; });

  REQUIRE(xdr.processCommand("T101700", true, false) == "T101700");
  REQUIRE(tuned.load() == 101700000u);
  REQUIRE(xdr.getFrequency() == 101700000u);

  REQUIRE(xdr.processCommand("T101700000", true, false) == "T101700");
  REQUIRE(tuned.load() == 101700000u);

  // An out-of-band tune is rejected and answered with the *current*
  // frequency, so the client can tell "rejected" from "not yet applied"
  // (the server used to answer with silence).
  REQUIRE(xdr.processCommand("T14900", true, false) == "T101700");
  REQUIRE(tuned.load() == 101700000u);
  REQUIRE(xdr.getFrequency() == 101700000u);

  REQUIRE(xdr.processCommand("Y77", true, false) == "Y77");
  REQUIRE(volume.load() == 77);
  REQUIRE(xdr.getVolume() == 77);

  REQUIRE(xdr.processCommand("W250000", true, false) == "W250000");
  REQUIRE(xdr.getBandwidth() == 250000);

  // 'W' is clamped to the widest bandwidth the tuner can actually measure.
  // Beyond the sample rate the signal-meter estimator's side-band window
  // wraps back over the channel and the meter measures the signal as its own
  // noise floor.
  REQUIRE(xdr.processCommand("W999999999", true, false) == "W256000");
  REQUIRE(xdr.getBandwidth() == 256000);
  REQUIRE(xdr.processCommand("W-1", true, false) == "W0");
  REQUIRE(xdr.getBandwidth() == 0);
  REQUIRE(xdr.processCommand("W250000", true, false) == "W250000");
  REQUIRE(xdr.getBandwidth() == 250000);

  REQUIRE(xdr.processCommand("A3", true, false) == "A3");
  REQUIRE(agc.load() == 3);
  REQUIRE(xdr.getAGCMode() == 3);

  REQUIRE(xdr.processCommand("I250,1", true, false) == "I250,1");
  REQUIRE(sampleInterval.load() == 250);
  REQUIRE(detector.load() == 1);

  REQUIRE(xdr.processCommand("B1", true, false) == "B1");
  REQUIRE(mono.load());

  // Stereo blend live-control via 'Fb<n>'.
  std::atomic<int> blendMode{-1};
  xdr.setBlendModeCallback([&](int b) { blendMode = b; });
  REQUIRE(xdr.processCommand("Fb0", true, false) == "Fb0");
  REQUIRE(blendMode.load() == 0);
  REQUIRE(xdr.processCommand("Fb2", true, false) == "Fb2");
  REQUIRE(blendMode.load() == 2);
  REQUIRE(xdr.processCommand("Fb1", true, false) == "Fb1");
  REQUIRE(blendMode.load() == 1);
  // Out-of-range arg is clamped.
  REQUIRE(xdr.processCommand("Fb9", true, false) == "Fb2");
  REQUIRE(blendMode.load() == 2);
  // Unknown F-subcommand returns empty without touching the callback.
  blendMode = 7;
  REQUIRE(xdr.processCommand("Fz5", true, false).empty());
  REQUIRE(blendMode.load() == 7);

  REQUIRE(xdr.processCommand("x", true, false) == "OK");
  REQUIRE(xdr.processCommand("X", true, false) == "X");
  REQUIRE(startCalls.load() == 1);
  REQUIRE(stopCalls.load() == 1);
}

TEST_CASE("XDR scan commands are clamped and exposed via consumeScanStart",
          "[xdr_unit]") {
  XDRServer xdr;
  xdr.setVerboseLogging(false);

  REQUIRE(xdr.processCommand("Sa63000", true, false).empty());
  REQUIRE(xdr.processCommand("Sb200000", true, false).empty());
  REQUIRE(xdr.processCommand("Sc2", true, false).empty());
  REQUIRE(xdr.processCommand("Sw999999", true, false).empty());
  REQUIRE(xdr.processCommand("Sz99", true, false).empty());
  REQUIRE(xdr.processCommand("Sm", true, false).empty());

  XDRServer::ScanConfig cfg{};
  REQUIRE(xdr.consumeScanStart(cfg));
  REQUIRE(cfg.startKHz == 64000);
  REQUIRE(cfg.stopKHz == 108000);
  REQUIRE(cfg.stepKHz == 5);
  REQUIRE(cfg.bandwidthHz == 400000);
  REQUIRE(cfg.antenna == 9);
  REQUIRE(cfg.continuous);
}

TEST_CASE("XDR guest sessions cannot change tuner state", "[xdr_unit]") {
  XDRServer xdr;
  xdr.setVerboseLogging(false);

  std::atomic<uint32_t> tuned{0};
  std::atomic<int> volume{0};
  std::atomic<int> startCalls{0};
  xdr.setFrequencyCallback([&](uint32_t f) { tuned = f; });
  xdr.setVolumeCallback([&](int v) { volume = v; });
  xdr.setStartCallback([&]() { startCalls++; });

  // A guest is not authenticated: every state-changing command is refused
  // with "a0" and never reaches the tuner.
  const char *forbidden[] = {"T101700", "Y77", "A3", "B1", "I250,1",
                             "x",     "X",   "W60000", "G20", "M1"};
  for (const char *cmd : forbidden) {
    REQUIRE(xdr.processCommand(cmd, false, true) == "a0");
  }
  REQUIRE(tuned.load() == 0u);
  REQUIRE(volume.load() == 0);
  REQUIRE(startCalls.load() == 0);

  // The spectral scan retunes the radio across the band, so a guest may not
  // start one — same rule as any other tuner command, and the same rule the
  // reference daemon applies (research/xdrd/xdrd.c:585). This costs the
  // default configuration nothing: with no password the challenge/response
  // accepts SHA1(salt), which is exactly what the reference client sends for
  // an empty password, so such a client is fully authenticated rather than a
  // guest.
  REQUIRE(xdr.processCommand("Sa87500", false, true) == "a0");
  REQUIRE(xdr.processCommand("S", false, true) == "a0");
  REQUIRE(xdr.processCommand("Sm", false, true) == "a0");
  // A guest still cannot reconfigure the tuner.
  REQUIRE(xdr.processCommand("T101700", false, true) == "a0");

  // Read-only queries still answer.
  REQUIRE(xdr.processCommand("N", false, true).rfind("N", 0) == 0);
  // The "o" line reports the real number of connected clients. This test
  // drives processCommand() directly without a socket, so nobody is
  // connected.
  REQUIRE(xdr.processCommand("o", false, true) == "o0,0");
  REQUIRE(xdr.processCommand("I", false, true).rfind("I", 0) == 0);

  // An authenticated session gets the same commands through.
  REQUIRE(xdr.processCommand("T101700", true, false) == "T101700");
  REQUIRE(tuned.load() == 101700000u);
}

TEST_CASE("XDR tune resets the RDS PI history", "[xdr_unit]") {
  XDRServer xdr;
  xdr.setVerboseLogging(false);

  // Feed enough clean groups for the PI to be reported.
  for (int i = 0; i < 8; i++) {
    xdr.updateRDS(0x1234, 0xABCD, 0x1111, 0x2222, 0x00);
  }
  {
    std::lock_guard<std::mutex> lock(xdr.m_rdsMutex);
    REQUIRE(xdr.m_piLastValue == 0x1234);
    REQUIRE(xdr.m_piFill > 0);
  }

  // A tune must clear the previous station's PI history.
  REQUIRE(xdr.processCommand("T101700", true, false) == "T101700");
  {
    std::lock_guard<std::mutex> lock(xdr.m_rdsMutex);
    REQUIRE(xdr.m_piLastValue == 0xFFFF);
    REQUIRE(xdr.m_piFill == 0);
  }

  // setFrequencyState() must also reset when it is told about a new
  // frequency, even though m_frequency was already updated by the command.
  for (int i = 0; i < 8; i++) {
    xdr.updateRDS(0x5678, 0xABCD, 0x1111, 0x2222, 0x00);
  }
  {
    std::lock_guard<std::mutex> lock(xdr.m_rdsMutex);
    REQUIRE(xdr.m_piLastValue == 0x5678);
  }
  xdr.setFrequencyState(90900000u);
  {
    std::lock_guard<std::mutex> lock(xdr.m_rdsMutex);
    REQUIRE(xdr.m_piLastValue == 0xFFFF);
    REQUIRE(xdr.m_piFill == 0);
  }
}

TEST_CASE("XDR updateRDS suppresses groups with block B errors", "[xdr_unit]") {
  XDRServer xdr;
  xdr.setVerboseLogging(false);

  xdr.updateRDS(0x1111, 0xABCD, 0x2222, 0x3333, 0x00);
  xdr.updateRDS(0x1111, 0xBBBB, 0x4444, 0x5555, 0x10); // block B error

  std::lock_guard<std::mutex> lock(xdr.m_rdsMutex);
  bool sawClean = false;
  bool sawErrored = false;
  for (const auto &entry : xdr.m_rdsQueue) {
    if (entry.second == "RABCD2222333300") {
      sawClean = true;
    }
    if (entry.second == "RBBBB4444555510") {
      sawErrored = true;
    }
  }

  REQUIRE(sawClean);
  REQUIRE_FALSE(sawErrored);
}

TEST_CASE("XDR stop joins client threads and clears thread registry",
          "[xdr_unit]") {
#if defined(_WIN32)
  WinSockInit wsa;
#endif
  const uint16_t port = reserveLoopbackPort();
  if (port == 0) {
    SKIP("Loopback sockets are unavailable in this environment");
  }

  XDRServer xdr(port);
  xdr.setVerboseLogging(false);
  REQUIRE(xdr.start());

  const int sock = connectLoopback(port);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  REQUIRE_FALSE(xdr.m_clientThreads.empty());

  xdr.stop();
  REQUIRE(xdr.m_clientThreads.empty());
  REQUIRE(xdr.m_clientSockets.empty());

  closeSock(sock);
}
