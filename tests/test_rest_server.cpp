#include "catch_compat.h"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "rest_server.h"

namespace {

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
  ::close(s);
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
    ::close(s);
    return {};
  }
  ::send(s, raw.data(), raw.size(), 0);
  std::string resp;
  char buf[1024];
  for (;;) {
    const auto n = ::recv(s, buf, sizeof(buf), 0);
    if (n <= 0) break;
    resp.append(buf, static_cast<size_t>(n));
  }
  ::close(s);
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
