#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "rtl_tcp_client.h"
#include "xdr_server.h"

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
#include <sys/time.h>
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

void setRecvTimeoutMs(int sock, int timeoutMs) {
#if defined(_WIN32)
  const DWORD timeout = timeoutMs > 0 ? static_cast<DWORD>(timeoutMs) : 0;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout),
             sizeof(timeout));
#else
  struct timeval tv;
  tv.tv_sec = timeoutMs / 1000;
  tv.tv_usec = (timeoutMs % 1000) * 1000;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

bool sendAll(int sock, const uint8_t *data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    const int n =
        send(sock, reinterpret_cast<const char *>(data + sent),
             static_cast<int>(len - sent), 0);
    if (n <= 0) {
      return false;
    }
    sent += static_cast<size_t>(n);
  }
  return true;
}

bool recvLine(int sock, std::string &line, size_t maxLen = 512) {
  line.clear();
  while (line.size() < maxLen) {
    char ch = '\0';
    const int n = recv(sock, &ch, 1, 0);
    if (n <= 0) {
      return !line.empty();
    }
    if (ch == '\n') {
      return true;
    }
    if (ch != '\r') {
      line.push_back(ch);
    }
  }
  return true;
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

bool waitForPrefixLine(int sock, const std::string &prefix,
                       std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::string line;
  while (std::chrono::steady_clock::now() < deadline) {
    if (!recvLine(sock, line)) {
      continue;
    }
    if (line.rfind(prefix, 0) == 0) {
      return true;
    }
  }
  return false;
}

bool waitForExactLine(int sock, const std::string &expected,
                      std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::string line;
  while (std::chrono::steady_clock::now() < deadline) {
    if (!recvLine(sock, line)) {
      continue;
    }
    if (line == expected) {
      return true;
    }
  }
  return false;
}

} // namespace

TEST_CASE("Protocol test harness baseline", "[protocol]") {
  REQUIRE(true);
}

TEST_CASE("RTLTCPClient connect fails when server is unavailable",
          "[protocol][rtl_tcp]") {
  const uint16_t port = reserveLoopbackPort();
  if (port == 0) {
    SKIP("Loopback sockets are unavailable in this environment");
  }
  RTLTCPClient client("127.0.0.1", port);
  REQUIRE_FALSE(client.connect());
}

TEST_CASE("RTLTCPClient disconnected methods are safe", "[protocol][rtl_tcp]") {
  RTLTCPClient client("127.0.0.1", 1234);
  REQUIRE(client.getSampleRate() == 1024000u);
  REQUIRE(client.getFrequency() == 0u);

  uint8_t iq[4] = {};
  REQUIRE(client.readIQ(iq, 2) == 0);
  REQUIRE_FALSE(client.setFrequency(90000000));
  REQUIRE_FALSE(client.setSampleRate(256000));
  REQUIRE_FALSE(client.setFrequencyCorrection(10));
  REQUIRE_FALSE(client.setGainMode(true));
  REQUIRE_FALSE(client.setGain(100));
  REQUIRE_FALSE(client.setAGC(true));

  client.disconnect();
}

TEST_CASE("RTLTCPClient command and IQ handling with mock rtl_tcp server",
          "[protocol][rtl_tcp]") {
#if defined(_WIN32)
  WinSockInit wsa;
#endif
  const uint16_t port = reserveLoopbackPort();
  if (port == 0) {
    SKIP("Loopback sockets are unavailable in this environment");
  }
  std::atomic<bool> serverReady{false};
  std::atomic<bool> serverOk{false};

  std::thread server([&]() {
    int listenSock = socket(AF_INET, SOCK_STREAM, 0);
    if (listenSock < 0) {
      return;
    }
    const int opt = 1;
#if defined(_WIN32)
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char *>(&opt), sizeof(opt));
#else
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    sockaddr_in addr{};
#if defined(__APPLE__)
    addr.sin_len = sizeof(addr);
#endif
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (bind(listenSock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 ||
        listen(listenSock, 1) != 0) {
      closeSock(listenSock);
      return;
    }
    serverReady = true;

    sockaddr_in clientAddr{};
    SocketLen clientLen = sizeof(clientAddr);
    int clientSock =
        accept(listenSock, reinterpret_cast<sockaddr *>(&clientAddr), &clientLen);
    if (clientSock < 0) {
      closeSock(listenSock);
      return;
    }

    const uint8_t header[12] = {'R', 'T', 'L', '0', 0, 0, 0, 0, 0, 0, 0, 0};
    if (!sendAll(clientSock, header, sizeof(header))) {
      closeSock(clientSock);
      closeSock(listenSock);
      return;
    }

    std::vector<uint8_t> cmd(25, 0);
    size_t readBytes = 0;
    while (readBytes < cmd.size()) {
      const int n = recv(clientSock, reinterpret_cast<char *>(cmd.data() + readBytes),
                         static_cast<int>(cmd.size() - readBytes), 0);
      if (n <= 0) {
        closeSock(clientSock);
        closeSock(listenSock);
        return;
      }
      readBytes += static_cast<size_t>(n);
    }

    const auto netU32 = [&](size_t off) {
      uint32_t v = 0;
      std::memcpy(&v, cmd.data() + off, sizeof(v));
      return ntohl(v);
    };

    const bool cmdsOk = (cmd[0] == 0x01 && netU32(1) == 101700000u &&
                         cmd[5] == 0x02 && netU32(6) == 256000u &&
                         cmd[10] == 0x03 && netU32(11) == 1u &&
                         cmd[15] == 0x04 && netU32(16) == 330u &&
                         cmd[20] == 0x08 && netU32(21) == 1u);
    if (!cmdsOk) {
      closeSock(clientSock);
      closeSock(listenSock);
      return;
    }

    const uint8_t iqA[5] = {1, 2, 3, 4, 5};
    const uint8_t iqB[1] = {6};
    const bool iqSent = sendAll(clientSock, iqA, sizeof(iqA)) &&
                        sendAll(clientSock, iqB, sizeof(iqB));
    serverOk = iqSent;

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    closeSock(clientSock);
    closeSock(listenSock);
  });

  for (int i = 0; i < 50 && !serverReady.load(); i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  REQUIRE(serverReady.load());

  RTLTCPClient client("127.0.0.1", port);
  REQUIRE(client.connect());
  REQUIRE(client.setFrequency(101700000u));
  REQUIRE(client.setSampleRate(256000u));
  REQUIRE(client.setGainMode(true));
  REQUIRE(client.setGain(330u));
  REQUIRE(client.setAGC(true));

  uint8_t iq1[8] = {};
  const size_t s1 = client.readIQ(iq1, 2);
  REQUIRE(s1 == 2);
  REQUIRE(iq1[0] == 1);
  REQUIRE(iq1[1] == 2);
  REQUIRE(iq1[2] == 3);
  REQUIRE(iq1[3] == 4);

  uint8_t iq2[4] = {};
  const size_t s2 = client.readIQ(iq2, 1);
  REQUIRE(s2 == 1);
  REQUIRE(iq2[0] == 5);
  REQUIRE(iq2[1] == 6);

  client.disconnect();
  server.join();
  REQUIRE(serverOk.load());
}

TEST_CASE("XDRServer accepts guest auth and processes core commands",
          "[protocol][xdr]") {
#if defined(_WIN32)
  WinSockInit wsa;
#endif
  const uint16_t port = reserveLoopbackPort();
  if (port == 0) {
    SKIP("Loopback sockets are unavailable in this environment");
  }
  XDRServer server(port);
  server.setVerboseLogging(false);
  server.setGuestMode(true);

  std::atomic<uint32_t> tunedHz{0};
  std::atomic<int> volume{0};
  std::atomic<int> agc{0};
  std::atomic<int> sampleInterval{-1};
  std::atomic<int> detector{-1};
  std::atomic<bool> forceMono{false};
  std::atomic<int> startCalls{0};

  server.setFrequencyCallback(
      [&](uint32_t f) { tunedHz.store(f, std::memory_order_relaxed); });
  server.setVolumeCallback(
      [&](int v) { volume.store(v, std::memory_order_relaxed); });
  server.setAGCCallback([&](int m) {
    agc.store(m, std::memory_order_relaxed);
    return true;
  });
  server.setSamplingCallback([&](int i, int d) {
    sampleInterval.store(i, std::memory_order_relaxed);
    detector.store(d, std::memory_order_relaxed);
  });
  server.setForceMonoCallback(
      [&](bool mono) { forceMono.store(mono, std::memory_order_relaxed); });
  server.setStartCallback(
      [&]() { startCalls.fetch_add(1, std::memory_order_relaxed); });

  REQUIRE(server.start());

  int sock = connectLoopback(port);
  setRecvTimeoutMs(sock, 500);

  std::string line;
  REQUIRE(recvLine(sock, line));
  REQUIRE(line.size() == XDRServer::SALT_LENGTH);

  const std::string badHash = "P0000000000000000000000000000000000000000\n";
  REQUIRE(sendAll(sock, reinterpret_cast<const uint8_t *>(badHash.data()),
                  badHash.size()));

  REQUIRE(waitForPrefixLine(sock, "a1", std::chrono::milliseconds(500)));
  REQUIRE(waitForPrefixLine(sock, "o0,1", std::chrono::milliseconds(500)));
  REQUIRE(waitForPrefixLine(sock, "I", std::chrono::milliseconds(500)));

  auto sendCmd = [&](const std::string &cmd) {
    const std::string withNl = cmd + "\n";
    REQUIRE(sendAll(sock, reinterpret_cast<const uint8_t *>(withNl.data()),
                    withNl.size()));
  };

  sendCmd("T101700");
  REQUIRE(waitForPrefixLine(sock, "T101700", std::chrono::milliseconds(700)));
  sendCmd("Y77");
  REQUIRE(waitForPrefixLine(sock, "Y77", std::chrono::milliseconds(700)));
  sendCmd("A3");
  REQUIRE(waitForPrefixLine(sock, "A3", std::chrono::milliseconds(700)));
  sendCmd("B1");
  REQUIRE(waitForPrefixLine(sock, "B1", std::chrono::milliseconds(700)));
  sendCmd("I250,1");
  REQUIRE(waitForPrefixLine(sock, "I250,1", std::chrono::milliseconds(700)));
  sendCmd("x");
  REQUIRE(waitForPrefixLine(sock, "OK", std::chrono::milliseconds(700)));

  REQUIRE(tunedHz.load() == 101700000u);
  REQUIRE(volume.load() == 77);
  REQUIRE(agc.load() == 3);
  REQUIRE(forceMono.load());
  REQUIRE(sampleInterval.load() == 250);
  REQUIRE(detector.load() == 1);
  REQUIRE(startCalls.load() >= 1);

  sendCmd("Sa87500");
  sendCmd("Sb108000");
  sendCmd("Sc100");
  sendCmd("Sw60000");
  sendCmd("Sz2");
  sendCmd("S");
  std::this_thread::sleep_for(std::chrono::milliseconds(40));

  XDRServer::ScanConfig scan{};
  REQUIRE(server.consumeScanStart(scan));
  REQUIRE(scan.startKHz == 87500);
  REQUIRE(scan.stopKHz == 108000);
  REQUIRE(scan.stepKHz == 100);
  REQUIRE(scan.bandwidthHz == 60000);
  REQUIRE(scan.antenna == 2);
  REQUIRE_FALSE(scan.continuous);

  closeSock(sock);
  server.stop();
}

TEST_CASE("XDRServer RDS forwarding requires error-free block B",
          "[protocol][xdr][rds]") {
  const uint16_t port = reserveLoopbackPort();
  if (port == 0) {
    SKIP("Loopback sockets are unavailable in this environment");
  }
  XDRServer server(port);
  server.setVerboseLogging(false);
  server.setGuestMode(true);
  REQUIRE(server.start());

  int sock = connectLoopback(port);
  setRecvTimeoutMs(sock, 250);

  std::string line;
  REQUIRE(recvLine(sock, line));
  REQUIRE(line.size() == XDRServer::SALT_LENGTH);
  const std::string badHash = "P0000000000000000000000000000000000000000\n";
  REQUIRE(sendAll(sock, reinterpret_cast<const uint8_t *>(badHash.data()),
                  badHash.size()));
  REQUIRE(waitForPrefixLine(sock, "a1", std::chrono::milliseconds(500)));

  // Let handshake chatter drain so RDS checks are deterministic.
  std::this_thread::sleep_for(std::chrono::milliseconds(60));

  // Block B clean -> R line should be forwarded.
  server.updateRDS(0x2222, 0xABCD, 0x1111, 0x2222, 0x00);
  REQUIRE(waitForExactLine(sock, "RABCD1111222200",
                           std::chrono::milliseconds(700)));

  // Block B has errors (bits 5:4 non-zero) -> R line should be suppressed.
  server.updateRDS(0x3333, 0xBBBB, 0x3333, 0x4444, 0x10);
  REQUIRE_FALSE(waitForExactLine(sock, "RBBBB3333444410",
                                 std::chrono::milliseconds(400)));

  closeSock(sock);
  server.stop();
}
