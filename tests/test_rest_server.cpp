#include "catch_compat.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#define CLOSESOCKET closesocket
using SocketDataLen = int;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#define CLOSESOCKET ::close
using SocketDataLen = size_t;
#endif

#include "rest_server.h"

namespace {

#if defined(_WIN32)
// Winsock must be initialized before any socket call; one static instance
// covers the whole test process.
struct WinsockInit {
  WinsockInit() {
    WSADATA d;
    WSAStartup(MAKEWORD(2, 2), &d);
  }
  ~WinsockInit() { WSACleanup(); }
};
const WinsockInit g_winsockInit;
#endif

// Bind an ephemeral port to discover a free one, then release it for the
// server to reuse (SO_REUSEADDR is set by RestServer).
uint16_t pickFreePort() {
  int s = ::socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE(s >= 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  REQUIRE(::bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
  socklen_t len = sizeof(addr);
  REQUIRE(::getsockname(s, reinterpret_cast<sockaddr *>(&addr), &len) == 0);
  const uint16_t port = ntohs(addr.sin_port);
  CLOSESOCKET(s);
  return port;
}

std::string httpRequest(uint16_t port, const std::string &raw) {
  int s = ::socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) return {};
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  if (::connect(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    CLOSESOCKET(s);
    return {};
  }
  ::send(s, raw.data(), static_cast<SocketDataLen>(raw.size()), 0);
  std::string resp;
  char buf[1024];
  for (;;) {
    const auto n = ::recv(s, buf, static_cast<SocketDataLen>(sizeof(buf)), 0);
    if (n <= 0) break;
    resp.append(buf, static_cast<size_t>(n));
  }
  CLOSESOCKET(s);
  return resp;
}

std::string body(const std::string &resp) {
  const size_t p = resp.find("\r\n\r\n");
  return p == std::string::npos ? std::string() : resp.substr(p + 4);
}

} // namespace

TEST_CASE("RestServer applies query-string params and reports status",
          "[rest]") {
  std::atomic<uint32_t> freq{0};
  std::atomic<int> lna{-1};
  std::atomic<double> gain{0.0};
  std::atomic<bool> mono{false};

  RestServer::Controls c;
  c.setFrequencyHz = [&](uint32_t hz) {
    freq.store(hz);
    return true;
  };
  c.setLnaState = [&](int s) {
    lna.store(s);
    return true;
  };
  c.setGainDb = [&](double db) {
    gain.store(db);
    return true;
  };
  c.setForceMono = [&](bool b) {
    mono.store(b);
    return true;
  };
  c.statusJson = [&]() {
    return std::string("{\"freq\":") + std::to_string(freq.load()) + "}";
  };

  const uint16_t port = pickFreePort();
  RestServer server("127.0.0.1", port, c);
  REQUIRE(server.start());
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  SECTION("freq_khz + lna applied via GET query") {
    const std::string resp = httpRequest(
        port, "GET /api/control?freq_khz=94900&lna=3 HTTP/1.1\r\n"
              "Host: x\r\nConnection: close\r\n\r\n");
    REQUIRE(resp.find("200 OK") != std::string::npos);
    REQUIRE(body(resp).find("\"applied\":2") != std::string::npos);
    REQUIRE(freq.load() == 94900000U);
    REQUIRE(lna.load() == 3);
  }

  SECTION("JSON POST body applies gain + force_mono") {
    const std::string payload = "{\"gain_db\": 28, \"force_mono\": true}";
    std::string req = "POST /api/control HTTP/1.1\r\nHost: x\r\n"
                      "Content-Type: application/json\r\nContent-Length: " +
                      std::to_string(payload.size()) +
                      "\r\nConnection: close\r\n\r\n" + payload;
    const std::string resp = httpRequest(port, req);
    REQUIRE(body(resp).find("\"applied\":2") != std::string::npos);
    REQUIRE(std::abs(gain.load() - 28.0) < 1e-6);
    REQUIRE(mono.load() == true);
  }

  SECTION("unknown key is rejected, not applied") {
    const std::string resp = httpRequest(
        port, "GET /api/control?bogus=1 HTTP/1.1\r\nConnection: close\r\n\r\n");
    REQUIRE(body(resp).find("\"ok\":false") != std::string::npos);
    REQUIRE(body(resp).find("\"rejected\"") != std::string::npos);
  }

  SECTION("status endpoint returns the provided JSON") {
    const std::string resp = httpRequest(
        port, "GET /api/status HTTP/1.1\r\nConnection: close\r\n\r\n");
    REQUIRE(resp.find("200 OK") != std::string::npos);
    REQUIRE(body(resp).find("\"freq\":") != std::string::npos);
  }

  server.stop();
}

TEST_CASE("RestServer serves other clients while one connection stalls",
          "[rest]") {
  RestServer::Controls c;
  c.statusJson = [&]() { return std::string("{\"ok\":true}"); };

  const uint16_t port = pickFreePort();
  RestServer server("127.0.0.1", port, c);
  REQUIRE(server.start());
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // A client that connects and then sends an incomplete request must not deny
  // the API to anyone else. Before connections were served on their own
  // threads, this stalled every other client for the whole receive timeout.
  int idle = ::socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE(idle >= 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  REQUIRE(::connect(idle, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
  const char partial[] = "GET /api/status HTTP/1.1\r\n";
  ::send(idle, partial, static_cast<int>(sizeof(partial) - 1), 0);

  // The second client must be served promptly.
  const auto start = std::chrono::steady_clock::now();
  const std::string resp = httpRequest(
      port, "GET /api/status HTTP/1.1\r\nConnection: close\r\n\r\n");
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();
  REQUIRE(resp.find("200 OK") != std::string::npos);
  REQUIRE(elapsed < 2000);

  CLOSESOCKET(idle);
  server.stop();
}

TEST_CASE("RestServer clamps an out-of-range bandwidth", "[rest]") {
  std::atomic<int> bandwidth{0};
  RestServer::Controls c;
  c.setBandwidthHz = [&](int hz) {
    bandwidth.store(hz);
    return true;
  };
  c.statusJson = [&]() { return "{}"; };

  const uint16_t port = pickFreePort();
  RestServer server("127.0.0.1", port, c);
  REQUIRE(server.start());
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // A bandwidth_khz large enough to overflow a 32-bit int multiply must be
  // clamped, not wrapped.
  const std::string resp = httpRequest(
      port, "GET /api/control?bandwidth_khz=3000000 HTTP/1.1\r\n"
            "Connection: close\r\n\r\n");
  REQUIRE(resp.find("200 OK") != std::string::npos);
  REQUIRE(bandwidth.load() <= 256000);
  REQUIRE(bandwidth.load() >= 0);

  // A normal bandwidth passes through unchanged.
  const std::string resp2 = httpRequest(
      port, "GET /api/control?bandwidth_khz=194 HTTP/1.1\r\n"
            "Connection: close\r\n\r\n");
  REQUIRE(resp2.find("200 OK") != std::string::npos);
  REQUIRE(bandwidth.load() == 194000);

  server.stop();
}

TEST_CASE("RestServer stops promptly while a client is still connected",
          "[rest]") {
  RestServer::Controls c;
  c.statusJson = [&]() { return "{}"; };

  const uint16_t port = pickFreePort();
  RestServer server("127.0.0.1", port, c);
  REQUIRE(server.start());
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // A connected but idle client. stop() must not wait out the full receive
  // timeout for it.
  int idle = ::socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE(idle >= 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  REQUIRE(::connect(idle, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  const auto start = std::chrono::steady_clock::now();
  server.stop();
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();
  REQUIRE(elapsed < 1000);

  CLOSESOCKET(idle);
}
