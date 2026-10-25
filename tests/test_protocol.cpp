#include "catch_compat.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "rtl_tcp_client.h"
#include "xdr_server.h"

#include <openssl/evp.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketLen = int;
using SocketResult = int;
using SocketDataLen = int;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using SocketLen = socklen_t;
using SocketResult = ssize_t;
using SocketDataLen = size_t;
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
    const SocketResult n =
        send(sock, reinterpret_cast<const char *>(data + sent),
             static_cast<SocketDataLen>(len - sent), 0);
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
    const SocketResult n = recv(sock, &ch, 1, 0);
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
  sockaddr_in addr{};
#if defined(__APPLE__)
  addr.sin_len = sizeof(addr);
#endif
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);

  // Retry the connect for a bounded window instead of failing on the first
  // attempt: the server thread may not have reached listen() yet, and that
  // race intermittently reddened CI (notably the macOS runner) with
  // "Failed to connect to 127.0.0.1". Poll until it accepts or ~2 s elapse.
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  for (;;) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(sock >= 0);
    if (connect(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0) {
      return sock;
    }
    closeSock(sock);
    if (std::chrono::steady_clock::now() >= deadline) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  REQUIRE(false); // server never became connectable within the timeout
  return -1;
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

bool waitForSocketClose(int sock, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  char ch = '\0';
  while (std::chrono::steady_clock::now() < deadline) {
    const SocketResult n = recv(sock, &ch, 1, 0);
    if (n == 0) {
      return true;
    }
    if (n < 0) {
#if defined(_WIN32)
      const int err = WSAGetLastError();
      if (err == WSAEWOULDBLOCK || err == WSAETIMEDOUT) {
        continue;
      }
#else
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
#endif
      return true;
    }
  }
  return false;
}

// The XDR challenge/response is SHA-1(salt + password), hex encoded — the
// same construction xdr-gtk uses (research/xdr-gtk/src/tuner-conn.c) and the
// same one XDRServer::computeSHA1 implements. The test needs it to drive a
// genuinely authenticated session.
std::string sha1Hex(const std::string &data) {
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digestLen = 0;
  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  if (ctx == nullptr) {
    return {};
  }
  if (EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr) != 1 ||
      EVP_DigestUpdate(ctx, data.data(), data.size()) != 1 ||
      EVP_DigestFinal_ex(ctx, digest, &digestLen) != 1) {
    EVP_MD_CTX_free(ctx);
    return {};
  }
  EVP_MD_CTX_free(ctx);

  static const char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(digestLen * 2);
  for (unsigned int i = 0; i < digestLen; i++) {
    out.push_back(kHex[digest[i] >> 4]);
    out.push_back(kHex[digest[i] & 0x0Fu]);
  }
  return out;
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
      const SocketResult n =
          recv(clientSock, reinterpret_cast<char *>(cmd.data() + readBytes),
               static_cast<SocketDataLen>(cmd.size() - readBytes), 0);
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

TEST_CASE("RTLTCPClient flushBuffers drops the stale IQ of the previous centre",
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

    // The stale block: samples that belong to the centre frequency the
    // tuner is about to leave.
    const uint8_t stale[4] = {0xAA, 0xAA, 0xAA, 0xAA};
    if (!sendAll(clientSock, stale, sizeof(stale))) {
      closeSock(clientSock);
      closeSock(listenSock);
      return;
    }
    // The fresh block follows later, so it is still in flight while the
    // client flushes.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const uint8_t fresh[4] = {0x55, 0x55, 0x55, 0x55};
    if (!sendAll(clientSock, fresh, sizeof(fresh))) {
      closeSock(clientSock);
      closeSock(listenSock);
      return;
    }
    serverOk = true;

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    closeSock(clientSock);
    closeSock(listenSock);
  });

  for (int i = 0; i < 50 && !serverReady.load(); i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  REQUIRE(serverReady.load());

  RTLTCPClient client("127.0.0.1", port);
  REQUIRE(client.connect());

  // Read one sample of the stale block, so the rest of it is left in the
  // socket buffer.
  uint8_t first[4] = {};
  REQUIRE(client.readIQ(first, 1) == 1);
  REQUIRE(first[0] == 0xAA);

  // The flush has to drop the samples of the previous centre, so the next
  // read returns the fresh block. Without the flush the stale samples are
  // still in the socket buffer and are read as if they belonged to the new
  // centre -- which is exactly what made the spectral scan measure the band
  // the tuner had just left.
  client.flushBuffers();

  uint8_t iq[8] = {};
  const size_t samples = client.readIQ(iq, 2);
  REQUIRE(samples == 2);
  REQUIRE(iq[0] == 0x55);
  REQUIRE(iq[1] == 0x55);
  REQUIRE(iq[2] == 0x55);
  REQUIRE(iq[3] == 0x55);

  client.disconnect();
  server.join();
  REQUIRE(serverOk.load());
}

TEST_CASE("RTLTCPClient rejects malformed rtl_tcp header",
          "[protocol][rtl_tcp]") {
#if defined(_WIN32)
  WinSockInit wsa;
#endif
  const uint16_t port = reserveLoopbackPort();
  if (port == 0) {
    SKIP("Loopback sockets are unavailable in this environment");
  }
  std::atomic<bool> serverReady{false};

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
    if (clientSock >= 0) {
      const uint8_t header[12] = {'B', 'A', 'D', '0', 0, 0, 0, 0, 0, 0, 0, 0};
      (void)sendAll(clientSock, header, sizeof(header));
      closeSock(clientSock);
    }
    closeSock(listenSock);
  });

  for (int i = 0; i < 50 && !serverReady.load(); i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  REQUIRE(serverReady.load());

  RTLTCPClient client("127.0.0.1", port);
  REQUIRE_FALSE(client.connect());
  server.join();
}

TEST_CASE("XDRServer guest sessions are read-only", "[protocol][xdr]") {
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
  setRecvTimeoutMs(sock, 2500);

  std::string line;
  REQUIRE(recvLine(sock, line));
  REQUIRE(line.size() == XDRServer::SALT_LENGTH);

  const std::string badHash = "P0000000000000000000000000000000000000000\n";
  REQUIRE(sendAll(sock, reinterpret_cast<const uint8_t *>(badHash.data()),
                  badHash.size()));

  // A failed authentication in guest mode is granted a read-only session.
  REQUIRE(waitForPrefixLine(sock, "a1", std::chrono::milliseconds(500)));
  REQUIRE(waitForPrefixLine(sock, "o0,1", std::chrono::milliseconds(500)));
  REQUIRE(waitForPrefixLine(sock, "I", std::chrono::milliseconds(500)));

  auto sendCmd = [&](const std::string &cmd) {
    const std::string withNl = cmd + "\n";
    REQUIRE(sendAll(sock, reinterpret_cast<const uint8_t *>(withNl.data()),
                    withNl.size()));
  };

  // Every state-changing command must be refused with "a0" (the protocol's
  // "not authorized" reply) and must not reach the tuner's callbacks.
  const std::vector<std::string> forbidden = {
      "T101700", "Y77", "A3", "B1", "I250,1", "x", "X", "W60000", "G20",
      "M1",     "D1",  "Z2", "Q50", "C1",  "V6",    "Fb2"};
  for (const std::string &cmd : forbidden) {
    sendCmd(cmd);
    REQUIRE(waitForPrefixLine(sock, "a0", std::chrono::milliseconds(700)));
  }

  REQUIRE(tunedHz.load() == 0u);
  REQUIRE(volume.load() == 0);
  REQUIRE(agc.load() == 0);
  REQUIRE_FALSE(forceMono.load());
  REQUIRE(sampleInterval.load() == -1);
  REQUIRE(detector.load() == -1);
  REQUIRE(startCalls.load() == 0);

  // A guest must not be able to arm a scan either.
  sendCmd("Sa87500");
  sendCmd("Sb108000");
  sendCmd("Sc100");
  sendCmd("Sw60000");
  sendCmd("Sz2");
  sendCmd("S");
  XDRServer::ScanConfig scan{};
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(700);
  while (std::chrono::steady_clock::now() < deadline) {
    if (server.consumeScanStart(scan)) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  REQUIRE_FALSE(server.consumeScanStart(scan));

  // Read-only commands still work: 'N' (pilot level), 'o' (online count) and
  // 'P' (authorization status) are queries, not state changes.
  sendCmd("N");
  REQUIRE(waitForPrefixLine(sock, "N", std::chrono::milliseconds(700)));
  sendCmd("o");
  REQUIRE(waitForPrefixLine(sock, "o0,1", std::chrono::milliseconds(700)));

  closeSock(sock);
  server.stop();
}

TEST_CASE("XDRServer authenticated sessions can command the tuner",
          "[protocol][xdr]") {
#if defined(_WIN32)
  WinSockInit wsa;
#endif
  const uint16_t port = reserveLoopbackPort();
  if (port == 0) {
    SKIP("Loopback sockets are unavailable in this environment");
  }
  const std::string password = "test123";
  XDRServer server(port);
  server.setVerboseLogging(false);
  server.setPassword(password);
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
  setRecvTimeoutMs(sock, 2500);

  std::string salt;
  REQUIRE(recvLine(sock, salt));
  REQUIRE(salt.size() == XDRServer::SALT_LENGTH);

  // The correct hash promotes the session to fully privileged.
  const std::string hash = sha1Hex(salt + password) + "\n";
  REQUIRE(sendAll(sock, reinterpret_cast<const uint8_t *>(hash.data()),
                  hash.size()));

  REQUIRE(waitForPrefixLine(sock, "a2", std::chrono::milliseconds(500)));
  REQUIRE(waitForPrefixLine(sock, "o1,0", std::chrono::milliseconds(500)));
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
  XDRServer::ScanConfig scan{};
  bool sawScanStart = false;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(700);
  while (std::chrono::steady_clock::now() < deadline) {
    if (server.consumeScanStart(scan)) {
      sawScanStart = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  REQUIRE(sawScanStart);
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
  setRecvTimeoutMs(sock, 2500);

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

TEST_CASE("XDRServer disconnects clients that send oversized unterminated lines",
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
  REQUIRE(server.start());

  int sock = connectLoopback(port);
  setRecvTimeoutMs(sock, 2500);

  std::string line;
  REQUIRE(recvLine(sock, line));
  REQUIRE(line.size() == XDRServer::SALT_LENGTH);
  const std::string badHash = "P0000000000000000000000000000000000000000\n";
  REQUIRE(sendAll(sock, reinterpret_cast<const uint8_t *>(badHash.data()),
                  badHash.size()));
  REQUIRE(waitForPrefixLine(sock, "a1", std::chrono::milliseconds(500)));
  REQUIRE(waitForPrefixLine(sock, "o0,1", std::chrono::milliseconds(500)));
  REQUIRE(waitForPrefixLine(sock, "I", std::chrono::milliseconds(500)));

  const std::string oversized(1500, 'A');
  REQUIRE(sendAll(sock, reinterpret_cast<const uint8_t *>(oversized.data()),
                  oversized.size()));
  REQUIRE(waitForSocketClose(sock, std::chrono::milliseconds(1000)));

  closeSock(sock);
  server.stop();
}

TEST_CASE("XDRServer disconnects FM-DX clients that send oversized unterminated lines",
          "[protocol][xdr][fmdx]") {
#if defined(_WIN32)
  WinSockInit wsa;
#endif
  const uint16_t port = reserveLoopbackPort();
  if (port == 0) {
    SKIP("Loopback sockets are unavailable in this environment");
  }
  XDRServer server(port);
  server.setVerboseLogging(false);
  REQUIRE(server.start());

  int sock = connectLoopback(port);
  setRecvTimeoutMs(sock, 2500);

  const std::string handshake = "x\n";
  REQUIRE(sendAll(sock, reinterpret_cast<const uint8_t *>(handshake.data()),
                  handshake.size()));
  REQUIRE(waitForExactLine(sock, "OK", std::chrono::milliseconds(500)));

  const std::string oversized(1500, 'x');
  REQUIRE(sendAll(sock, reinterpret_cast<const uint8_t *>(oversized.data()),
                  oversized.size()));
  REQUIRE(waitForSocketClose(sock, std::chrono::milliseconds(1000)));

  closeSock(sock);
  server.stop();
}

TEST_CASE("XDRServer rejects oversized FM-DX handshake lines",
          "[protocol][xdr][fmdx]") {
#if defined(_WIN32)
  WinSockInit wsa;
#endif
  const uint16_t port = reserveLoopbackPort();
  if (port == 0) {
    SKIP("Loopback sockets are unavailable in this environment");
  }
  XDRServer server(port);
  server.setVerboseLogging(false);
  REQUIRE(server.start());

  int sock = connectLoopback(port);
  setRecvTimeoutMs(sock, 2500);

  const std::string oversizedHandshake(20, 'x');
  const std::string payload = oversizedHandshake + "\n";
  REQUIRE(sendAll(sock, reinterpret_cast<const uint8_t *>(payload.data()),
                  payload.size()));
  REQUIRE(waitForSocketClose(sock, std::chrono::milliseconds(1000)));

  closeSock(sock);
  server.stop();
}

TEST_CASE("XDRServer rejects oversized XDR auth lines", "[protocol][xdr]") {
#if defined(_WIN32)
  WinSockInit wsa;
#endif
  const uint16_t port = reserveLoopbackPort();
  if (port == 0) {
    SKIP("Loopback sockets are unavailable in this environment");
  }
  XDRServer server(port);
  server.setVerboseLogging(false);
  REQUIRE(server.start());

  int sock = connectLoopback(port);
  setRecvTimeoutMs(sock, 2500);

  std::string line;
  REQUIRE(recvLine(sock, line));
  REQUIRE(line.size() == XDRServer::SALT_LENGTH);

  const std::string oversizedAuth = "P" + std::string(80, 'A') + "\n";
  REQUIRE(sendAll(sock, reinterpret_cast<const uint8_t *>(oversizedAuth.data()),
                  oversizedAuth.size()));
  REQUIRE(waitForSocketClose(sock, std::chrono::milliseconds(1000)));

  closeSock(sock);
  server.stop();
}

TEST_CASE("XDRServer serves the reference FM-DX startup sequence",
          "[protocol][xdr][fmdx]") {
#if defined(_WIN32)
  WinSockInit wsa;
#endif
  const uint16_t port = reserveLoopbackPort();
  if (port == 0) {
    SKIP("Loopback sockets are unavailable in this environment");
  }
  XDRServer server(port);
  server.setVerboseLogging(false);
  REQUIRE(server.start());

  std::atomic<uint32_t> tunedHz{0};
  std::atomic<int> startCalls{0};
  server.setFrequencyCallback(
      [&](uint32_t f) { tunedHz.store(f, std::memory_order_relaxed); });
  server.setStartCallback(
      [&]() { startCalls.fetch_add(1, std::memory_order_relaxed); });

  int sock = connectLoopback(port);
  setRecvTimeoutMs(sock, 2500);

  // The reference FM-DX client sends "x" exactly once, in its startup
  // handshake, and expects "\nOK\n" back (Dispatcher::cbStartup). It then
  // starts sending commands. The server used to swallow the "x" here, which
  // left the command loop thinking the tuner had never been started, so
  // every command was answered "ER" and the client could never tune.
  const std::string startup = "x\n";
  REQUIRE(sendAll(sock, reinterpret_cast<const uint8_t *>(startup.data()),
                  startup.size()));
  REQUIRE(waitForExactLine(sock, "OK", std::chrono::milliseconds(1000)));
  REQUIRE(startCalls.load() == 1);

  // A command right after the single startup line must be accepted.
  const std::string tune = "T101700\n";
  REQUIRE(sendAll(sock, reinterpret_cast<const uint8_t *>(tune.data()),
                  tune.size()));
  REQUIRE(waitForPrefixLine(sock, "OK", std::chrono::milliseconds(1000)));
  REQUIRE(tunedHz.load() == 101700000u);

  closeSock(sock);
  server.stop();
}

TEST_CASE("XDRServer rejects an out-of-band FM-DX tune", "[protocol][xdr][fmdx]") {
#if defined(_WIN32)
  WinSockInit wsa;
#endif
  const uint16_t port = reserveLoopbackPort();
  if (port == 0) {
    SKIP("Loopback sockets are unavailable in this environment");
  }
  XDRServer server(port);
  server.setVerboseLogging(false);
  REQUIRE(server.start());

  std::atomic<uint32_t> tunedHz{0};
  server.setFrequencyCallback(
      [&](uint32_t f) { tunedHz.store(f, std::memory_order_relaxed); });

  int sock = connectLoopback(port);
  setRecvTimeoutMs(sock, 2500);

  const std::string startup = "x\n";
  REQUIRE(sendAll(sock, reinterpret_cast<const uint8_t *>(startup.data()),
                  startup.size()));
  REQUIRE(waitForExactLine(sock, "OK", std::chrono::milliseconds(1000)));

  // "T10" is read as 10 kHz by parseFrequencyHz — far outside the FM
  // broadcast band. The XDR-GTK path has always rejected such a tune; the
  // FM-DX path did not, so it could drive the tuner off-band.
  const std::string badTune = "T10\n";
  REQUIRE(sendAll(sock, reinterpret_cast<const uint8_t *>(badTune.data()),
                  badTune.size()));
  REQUIRE(waitForExactLine(sock, "ER", std::chrono::milliseconds(1000)));
  REQUIRE(tunedHz.load() == 0u);

  closeSock(sock);
  server.stop();
}

TEST_CASE("XDRServer requires the password on the FM-DX path",
          "[protocol][xdr][fmdx]") {
#if defined(_WIN32)
  WinSockInit wsa;
#endif
  const uint16_t port = reserveLoopbackPort();
  if (port == 0) {
    SKIP("Loopback sockets are unavailable in this environment");
  }
  XDRServer server(port);
  server.setVerboseLogging(false);
  server.setPassword("secret123");
  server.setGuestMode(false);
  REQUIRE(server.start());

  std::atomic<uint32_t> tunedHz{0};
  server.setFrequencyCallback(
      [&](uint32_t f) { tunedHz.store(f, std::memory_order_relaxed); });

  // A client that sends only "x" and then a tune must not be able to
  // control the tuner: the password is checked on this path too.
  {
    int sock = connectLoopback(port);
    setRecvTimeoutMs(sock, 2500);
    const std::string startup = "x\n";
    REQUIRE(sendAll(sock, reinterpret_cast<const uint8_t *>(startup.data()),
                    startup.size()));
    REQUIRE(waitForExactLine(sock, "OK", std::chrono::milliseconds(1000)));

    // The server now asks for the salt/hash.
    std::string salt;
    REQUIRE(recvLine(sock, salt));
    REQUIRE(salt.size() == XDRServer::SALT_LENGTH);

    const std::string badHash = "P0000000000000000000000000000000000000000\n";
    REQUIRE(sendAll(sock, reinterpret_cast<const uint8_t *>(badHash.data()),
                    badHash.size()));
    REQUIRE(waitForPrefixLine(sock, "a0", std::chrono::milliseconds(1000)));

    const std::string tune = "T101700\n";
    REQUIRE(sendAll(sock, reinterpret_cast<const uint8_t *>(tune.data()),
                    tune.size()));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    REQUIRE(tunedHz.load() == 0u);
    closeSock(sock);
  }

  // With the correct hash the same sequence works.
  {
    int sock = connectLoopback(port);
    setRecvTimeoutMs(sock, 2500);
    const std::string startup = "x\n";
    REQUIRE(sendAll(sock, reinterpret_cast<const uint8_t *>(startup.data()),
                    startup.size()));
    REQUIRE(waitForExactLine(sock, "OK", std::chrono::milliseconds(1000)));

    std::string salt;
    REQUIRE(recvLine(sock, salt));
    REQUIRE(salt.size() == XDRServer::SALT_LENGTH);

    const std::string hash = sha1Hex(salt + "secret123") + "\n";
    REQUIRE(sendAll(sock, reinterpret_cast<const uint8_t *>(hash.data()),
                    hash.size()));

    const std::string tune = "T101700\n";
    REQUIRE(sendAll(sock, reinterpret_cast<const uint8_t *>(tune.data()),
                    tune.size()));
    REQUIRE(waitForPrefixLine(sock, "OK", std::chrono::milliseconds(1000)));
    REQUIRE(tunedHz.load() == 101700000u);
    closeSock(sock);
  }

  server.stop();
}

TEST_CASE("XDRServer reports the real number of connected clients",
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
  REQUIRE(server.start());

  int first = connectLoopback(port);
  setRecvTimeoutMs(first, 2500);
  std::string line;
  REQUIRE(recvLine(first, line)); // salt
  const std::string badHash = "P0000000000000000000000000000000000000000\n";
  REQUIRE(sendAll(first, reinterpret_cast<const uint8_t *>(badHash.data()),
                  badHash.size()));
  REQUIRE(waitForPrefixLine(first, "a1", std::chrono::milliseconds(1000)));
  REQUIRE(waitForPrefixLine(first, "o0,1", std::chrono::milliseconds(1000)));

  // A second guest joins: the count goes to two and the first client is
  // told about it. The server used to synthesise the line from the
  // *current* session's own role, so it always claimed exactly one user.
  int second = connectLoopback(port);
  setRecvTimeoutMs(second, 2500);
  REQUIRE(recvLine(second, line)); // salt
  REQUIRE(sendAll(second, reinterpret_cast<const uint8_t *>(badHash.data()),
                  badHash.size()));
  REQUIRE(waitForPrefixLine(second, "a1", std::chrono::milliseconds(1000)));
  REQUIRE(waitForPrefixLine(second, "o0,2", std::chrono::milliseconds(1000)));

  // The first client is notified of the new arrival.
  REQUIRE(waitForPrefixLine(first, "o0,2", std::chrono::milliseconds(1000)));

  // When the second client leaves the count drops again.
  closeSock(second);
  REQUIRE(waitForPrefixLine(first, "o0,1", std::chrono::milliseconds(2000)));

  closeSock(first);
  server.stop();
}

TEST_CASE("XDRServer accepts a pipelined FM-DX startup and auth hash",
          "[protocol][xdr][fmdx]") {
#if defined(_WIN32)
  WinSockInit wsa;
#endif
  const uint16_t port = reserveLoopbackPort();
  if (port == 0) {
    SKIP("Loopback sockets are unavailable in this environment");
  }
  XDRServer server(port);
  server.setVerboseLogging(false);
  server.setPassword("secret123");
  server.setGuestMode(false);
  REQUIRE(server.start());

  std::atomic<uint32_t> tunedHz{0};
  server.setFrequencyCallback(
      [&](uint32_t f) { tunedHz.store(f, std::memory_order_relaxed); });

  int sock = connectLoopback(port);
  setRecvTimeoutMs(sock, 2500);

  // The client sends the startup line and then the hash. If the server
  // reads the hash with a separate blocking read it would swallow the line
  // already sitting in the command buffer, and the client would hang
  // waiting for a reply that never comes.
  const std::string startup = "x\n";
  REQUIRE(sendAll(sock, reinterpret_cast<const uint8_t *>(startup.data()),
                  startup.size()));
  REQUIRE(waitForExactLine(sock, "OK", std::chrono::milliseconds(1000)));

  std::string salt;
  REQUIRE(recvLine(sock, salt));
  REQUIRE(salt.size() == XDRServer::SALT_LENGTH);

  // The hash and the first command arrive in one write.
  const std::string payload = sha1Hex(salt + "secret123") + "\nT101700\n";
  REQUIRE(sendAll(sock, reinterpret_cast<const uint8_t *>(payload.data()),
                  payload.size()));
  REQUIRE(waitForPrefixLine(sock, "OK", std::chrono::milliseconds(1000)));
  REQUIRE(tunedHz.load() == 101700000u);

  closeSock(sock);
  server.stop();
}

TEST_CASE("XDRServer: a bystander disconnect does not cancel a scan",
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
  server.setPassword("secret123");
  server.setGuestMode(false);
  REQUIRE(server.start());

  // The scan owner connects and authenticates, then arms a scan.
  int owner = connectLoopback(port);
  setRecvTimeoutMs(owner, 2500);
  std::string line;
  REQUIRE(recvLine(owner, line));
  REQUIRE(line.size() == XDRServer::SALT_LENGTH);
  const std::string hash = sha1Hex(line + "secret123") + "\n";
  REQUIRE(sendAll(owner, reinterpret_cast<const uint8_t *>(hash.data()),
                  hash.size()));
  REQUIRE(waitForPrefixLine(owner, "a2", std::chrono::milliseconds(1000)));

  const std::string arm = "S\n";
  REQUIRE(sendAll(owner, reinterpret_cast<const uint8_t *>(arm.data()),
                  arm.size()));
  // The command is processed on the client thread, so give it a moment to
  // reach the handler before asking whether the scan was armed.
  XDRServer::ScanConfig cfg{};
  bool armed = false;
  for (int i = 0; i < 20 && !armed; i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    armed = server.consumeScanStart(cfg);
  }
  REQUIRE(armed);

  // A second client authenticates and then disconnects. It never armed the
  // scan, so the owner's sweep must survive. (The bystander has to get past
  // the auth phase: a client that is rejected with "a0" returns before the
  // command loop, so it never reaches the disconnect handling at all.)
  int bystander = connectLoopback(port);
  setRecvTimeoutMs(bystander, 2500);
  REQUIRE(recvLine(bystander, line));
  const std::string bystanderHash = sha1Hex(line + "secret123") + "\n";
  REQUIRE(sendAll(bystander,
                  reinterpret_cast<const uint8_t *>(bystanderHash.data()),
                  bystanderHash.size()));
  REQUIRE(waitForPrefixLine(bystander, "a2", std::chrono::milliseconds(1000)));
  closeSock(bystander);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  REQUIRE_FALSE(server.consumeScanCancel());

  closeSock(owner);
  server.stop();
}

TEST_CASE("FM-DX client disconnect stops only a tuner it started",
          "[protocol][xdr][fmdx]") {
#if defined(_WIN32)
  WinSockInit wsa;
#endif
  // The FM-DX path stops the tuner when the client that started it goes
  // away. A client that attaches to a tuner that is already running
  // (--auto-start, or a tuner another client is using) must not cut the
  // programme when it disconnects: the listener would hear the station
  // restart, which reads as the station repeating itself.
  auto runScenario = [](uint16_t scenarioPort, bool tunerAlreadyRunning) {
    XDRServer server(scenarioPort);
    server.setVerboseLogging(false);
    std::atomic<int> startCalls{0};
    std::atomic<int> stopCalls{0};
    server.setStartCallback(
        [&]() { startCalls.fetch_add(1, std::memory_order_relaxed); });
    server.setStopCallback(
        [&]() { stopCalls.fetch_add(1, std::memory_order_relaxed); });
    server.setTunerActiveCallback([&]() { return tunerAlreadyRunning; });
    REQUIRE(server.start());

    int sock = connectLoopback(scenarioPort);
    setRecvTimeoutMs(sock, 2500);
    const std::string startup = "x\n";
    REQUIRE(sendAll(sock, reinterpret_cast<const uint8_t *>(startup.data()),
                    startup.size()));
    REQUIRE(waitForExactLine(sock, "OK", std::chrono::milliseconds(1000)));
    REQUIRE(startCalls.load() == 1);

    closeSock(sock);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    server.stop();
    return stopCalls.load();
  };

  const uint16_t ownerPort = reserveLoopbackPort();
  const uint16_t guestPort = reserveLoopbackPort();
  if (ownerPort == 0 || guestPort == 0) {
    SKIP("Loopback sockets are unavailable in this environment");
  }

  // The client started the tuner: its disconnect stops it, as the reference
  // protocol expects.
  REQUIRE(runScenario(ownerPort, false) == 1);

  // The tuner was already running: the client does not own it, so its
  // disconnect leaves the tuner alone.
  REQUIRE(runScenario(guestPort, true) == 0);
}

namespace {
bool sendText(int sock, const std::string &text) {
  return sendAll(sock, reinterpret_cast<const uint8_t *>(text.data()),
                 text.size());
}

// Connects an FM-DX client and completes its startup line.
int connectFmdx(uint16_t port) {
  int sock = connectLoopback(port);
  setRecvTimeoutMs(sock, 2500);
  REQUIRE(sendText(sock, "x\n"));
  REQUIRE(waitForExactLine(sock, "OK", std::chrono::milliseconds(1000)));
  return sock;
}
} // namespace

TEST_CASE("FM-DX client with a wrong password does not start the tuner",
          "[protocol][xdr][fmdx]") {
#if defined(_WIN32)
  WinSockInit wsa;
#endif
  const uint16_t port = reserveLoopbackPort();
  if (port == 0) {
    SKIP("Loopback sockets are unavailable in this environment");
  }
  XDRServer server(port);
  server.setVerboseLogging(false);
  server.setPassword("secret123");
  std::atomic<int> startCalls{0};
  std::atomic<int> stopCalls{0};
  server.setStartCallback([&]() { startCalls++; });
  server.setStopCallback([&]() { stopCalls++; });
  server.setTunerActiveCallback([&]() { return startCalls > stopCalls; });
  REQUIRE(server.start());

  int sock = connectFmdx(port);
  std::string salt;
  REQUIRE(recvLine(sock, salt));
  REQUIRE(sendText(sock, std::string(40, '0') + "\n"));
  REQUIRE(waitForPrefixLine(sock, "a0", std::chrono::milliseconds(1000)));
  closeSock(sock);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  REQUIRE(startCalls.load() == 0);

  // The correct hash starts it.
  sock = connectFmdx(port);
  REQUIRE(recvLine(sock, salt));
  REQUIRE(sendText(sock, sha1Hex(salt + "secret123") + "\n"));
  for (int i = 0; i < 40 && startCalls.load() == 0; i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  REQUIRE(startCalls.load() == 1);
  closeSock(sock);
  server.stop();
}

TEST_CASE("FM-DX tuner stops only when the last client using it leaves",
          "[protocol][xdr][fmdx]") {
#if defined(_WIN32)
  WinSockInit wsa;
#endif
  const uint16_t port = reserveLoopbackPort();
  if (port == 0) {
    SKIP("Loopback sockets are unavailable in this environment");
  }
  XDRServer server(port);
  server.setVerboseLogging(false);
  std::atomic<bool> running{false};
  std::atomic<int> stopCalls{0};
  server.setStartCallback([&]() { running = true; });
  server.setStopCallback([&]() {
    running = false;
    stopCalls++;
  });
  server.setTunerActiveCallback([&]() { return running.load(); });
  REQUIRE(server.start());

  // A starts the tuner, B joins it.
  int a = connectFmdx(port);
  int b = connectFmdx(port);
  REQUIRE(running.load());

  // A leaves: B is still listening, so the tuner keeps running.
  closeSock(a);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  REQUIRE(stopCalls.load() == 0);
  REQUIRE(running.load());

  // B was the last one: the tuner an FM-DX client started is stopped.
  closeSock(b);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  REQUIRE(stopCalls.load() == 1);
  REQUIRE_FALSE(running.load());
  server.stop();
}

TEST_CASE("FM-DX client that never finishes its startup line is dropped",
          "[protocol][xdr][fmdx]") {
#if defined(_WIN32)
  WinSockInit wsa;
#endif
  const uint16_t port = reserveLoopbackPort();
  if (port == 0) {
    SKIP("Loopback sockets are unavailable in this environment");
  }
  XDRServer server(port);
  server.setVerboseLogging(false);
  REQUIRE(server.start());

  int sock = connectLoopback(port);
  REQUIRE(sendText(sock, "x"));
  // The handshake deadline is 5 s; the server must close well before 8 s.
  setRecvTimeoutMs(sock, 8000);
  uint8_t byte = 0;
  const auto t0 = std::chrono::steady_clock::now();
  const auto n = recv(sock, reinterpret_cast<char *>(&byte), 1, 0);
  const auto waited = std::chrono::steady_clock::now() - t0;
  REQUIRE(n == 0);
  REQUIRE(waited < std::chrono::milliseconds(7000));
  closeSock(sock);
  server.stop();
}

TEST_CASE("XDRServer sends the salt first even when a peer leaves meanwhile",
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
  REQUIRE(server.start());

  // A peer with an established session.
  int peer = connectLoopback(port);
  setRecvTimeoutMs(peer, 2500);
  std::string line;
  REQUIRE(recvLine(peer, line));
  REQUIRE(sendText(peer, sha1Hex(line) + "\n"));
  REQUIRE(waitForPrefixLine(peer, "a2", std::chrono::milliseconds(1000)));

  // A newcomer connects, and the peer leaves while the newcomer is still in
  // protocol detection. The peer's departure broadcasts an "o" line, which
  // must not reach the newcomer ahead of its salt.
  int newcomer = connectLoopback(port);
  setRecvTimeoutMs(newcomer, 2500);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  closeSock(peer);

  REQUIRE(recvLine(newcomer, line));
  REQUIRE(line.size() == XDRServer::SALT_LENGTH);
  REQUIRE(line[0] != 'o');
  closeSock(newcomer);
  server.stop();
}
