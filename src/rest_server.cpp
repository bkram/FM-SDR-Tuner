#include "rest_server.h"

#include "tuning_limits.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketLen = int;
#define CLOSESOCKET closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using SocketLen = socklen_t;
#define CLOSESOCKET ::close
#endif

#include "socket_subsystem.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>

namespace {

// How long a client may take to deliver its request before the server gives
// up on it. The API is small and local; anything slower than this is not a
// real client, and holding the accept thread for it would deny the API to
// every other client.
constexpr int kClientRecvTimeoutMs = 5000;

// The widest channel bandwidth the tuner accepts. The signal-meter estimator
// analyses the channel with an FFT whose length is capped at 8192 points;
// beyond roughly the sample rate the side-band window wraps back over the
// channel and the meter measures the signal as its own noise floor. The
// tuner runs at 256 kHz, so a bandwidth at or above the sample rate is never
// a meaningful request.
constexpr long long kMaxBandwidthHz = 256000;

// Clamp a requested bandwidth into the range the tuner can actually
// measure. Applied on every ingress so the raw value never reaches the
// signal-meter estimator.
int clampBandwidthHz(long long hz) {
  if (hz < 0) return 0;
  if (hz > kMaxBandwidthHz) return static_cast<int>(kMaxBandwidthHz);
  return static_cast<int>(hz);
}

std::string urlDecode(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '%' && i + 2 < s.size()) {
      auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
      };
      const int hi = hex(s[i + 1]);
      const int lo = hex(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    if (s[i] == '+') {
      out.push_back(' ');
    } else {
      out.push_back(s[i]);
    }
  }
  return out;
}

std::string trim(const std::string &s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
  return s.substr(a, b - a);
}

// Parse "a=b&c=d" form (query string or urlencoded body).
void parseFormParams(const std::string &body,
                     std::vector<std::pair<std::string, std::string>> &out) {
  size_t pos = 0;
  while (pos < body.size()) {
    size_t amp = body.find('&', pos);
    if (amp == std::string::npos) amp = body.size();
    const std::string pair = body.substr(pos, amp - pos);
    const size_t eq = pair.find('=');
    if (eq != std::string::npos) {
      out.emplace_back(urlDecode(pair.substr(0, eq)), urlDecode(pair.substr(eq + 1)));
    } else if (!pair.empty()) {
      out.emplace_back(urlDecode(pair), "");
    }
    pos = amp + 1;
  }
}

// Minimal flat-JSON object parser: {"key": <number|true|false|"string">, ...}.
// Sufficient for control payloads; nested objects/arrays are ignored.
void parseFlatJson(const std::string &body,
                   std::vector<std::pair<std::string, std::string>> &out) {
  size_t i = 0;
  const size_t n = body.size();
  auto skipWs = [&]() {
    while (i < n && std::isspace(static_cast<unsigned char>(body[i]))) i++;
  };
  skipWs();
  if (i >= n || body[i] != '{') return;
  i++;
  while (i < n) {
    skipWs();
    if (i < n && body[i] == '}') break;
    if (i >= n || body[i] != '"') break;
    i++;
    std::string key;
    while (i < n && body[i] != '"') key.push_back(body[i++]);
    if (i < n) i++; // closing quote
    skipWs();
    if (i < n && body[i] == ':') i++;
    skipWs();
    std::string value;
    if (i < n && body[i] == '"') {
      i++;
      while (i < n && body[i] != '"') value.push_back(body[i++]);
      if (i < n) i++;
    } else {
      while (i < n && body[i] != ',' && body[i] != '}' &&
             !std::isspace(static_cast<unsigned char>(body[i]))) {
        value.push_back(body[i++]);
      }
    }
    out.emplace_back(trim(key), trim(value));
    skipWs();
    if (i < n && body[i] == ',') {
      i++;
      continue;
    }
  }
}

bool toBool(const std::string &v, bool &out) {
  std::string s = v;
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (s == "1" || s == "true" || s == "on" || s == "yes") {
    out = true;
    return true;
  }
  if (s == "0" || s == "false" || s == "off" || s == "no") {
    out = false;
    return true;
  }
  return false;
}

bool toInt(const std::string &v, long &out) {
  if (v.empty()) return false;
  errno = 0;
  char *end = nullptr;
  const long parsed = std::strtol(v.c_str(), &end, 10);
  if (end == v.c_str() || errno != 0) return false;
  out = parsed;
  return true;
}

bool toDouble(const std::string &v, double &out) {
  if (v.empty()) return false;
  errno = 0;
  char *end = nullptr;
  const double parsed = std::strtod(v.c_str(), &end);
  if (end == v.c_str() || errno != 0) return false;
  out = parsed;
  return true;
}

// A peer that disconnects mid-response raises SIGPIPE, whose default
// disposition is to terminate the process. MSG_NOSIGNAL (POSIX) keeps the
// error local to the send() call, which returns EPIPE instead.
// The platform's socket type: SOCKET on Windows, an int descriptor on POSIX.
#if defined(_WIN32)
inline SOCKET nativeSocket(SocketHandle handle) {
  return static_cast<SOCKET>(handle);
}
#else
inline int nativeSocket(SocketHandle handle) {
  return static_cast<int>(handle);
}
#endif

int sendNoSigPipe(SocketHandle sock, const char *data, size_t len) {
#if defined(_WIN32)
  return ::send(nativeSocket(sock), data, static_cast<int>(len), 0);
#elif defined(MSG_NOSIGNAL)
  return static_cast<int>(
      ::send(nativeSocket(sock), data, len, MSG_NOSIGNAL));
#else
  return static_cast<int>(::send(nativeSocket(sock), data, len, 0));
#endif
}

// SO_NOSIGPIPE is the BSD/macOS equivalent of MSG_NOSIGNAL: it suppresses
// SIGPIPE for the socket rather than for the individual call.
void configureSendNoSigPipe(SocketHandle sock) {
#if defined(SO_NOSIGPIPE)
  const int opt = 1;
  setsockopt(nativeSocket(sock), SOL_SOCKET, SO_NOSIGPIPE,
             reinterpret_cast<const char *>(&opt), sizeof(opt));
#else
  (void)sock;
#endif
}

// Bound how long a single client may hold the server up while it dribbles in
// a request. Without this, one idle connection stalls every other client.
void setRecvTimeoutMs(SocketHandle sock, int timeoutMs) {
#if defined(_WIN32)
  const DWORD timeout = timeoutMs > 0 ? static_cast<DWORD>(timeoutMs) : 0;
  setsockopt(nativeSocket(sock), SOL_SOCKET, SO_RCVTIMEO,
             reinterpret_cast<const char *>(&timeout), sizeof(timeout));
#else
  struct timeval tv;
  tv.tv_sec = timeoutMs / 1000;
  tv.tv_usec = (timeoutMs % 1000) * 1000;
  setsockopt(nativeSocket(sock), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

void sendResponse(SocketHandle sock, int status,
                  const std::string &statusText, const std::string &body) {
  std::ostringstream oss;
  oss << "HTTP/1.1 " << status << " " << statusText << "\r\n"
      << "Content-Type: application/json\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Access-Control-Allow-Origin: *\r\n"
      << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
      << "Access-Control-Allow-Headers: Content-Type\r\n"
      << "Connection: close\r\n\r\n"
      << body;
  const std::string out = oss.str();
  size_t sent = 0;
  while (sent < out.size()) {
    const auto n = sendNoSigPipe(sock, out.data() + sent, out.size() - sent);
    if (n <= 0) break;
    sent += static_cast<size_t>(n);
  }
}

} // namespace

RestServer::RestServer(std::string bindAddress, uint16_t port, Controls controls)
    : m_bindAddress(std::move(bindAddress)), m_port(port),
      m_controls(std::move(controls)) {}

RestServer::~RestServer() { stop(); }

bool RestServer::start() {
  if (m_running.load()) return true;
#if defined(_WIN32)
  // Initialize Winsock so the REST server is self-sufficient and doesn't rely
  // on the XDR server (or anyone else) having started the socket subsystem
  // first. The start is reference-counted and shared with the other socket
  // users, and it is released when the process exits.
  {
    static SocketSubsystemLease lease;
    if (!lease.ok()) {
      std::cerr << "[REST] WSAStartup failed\n";
      return false;
    }
  }
#endif
  m_serverSocket = static_cast<SocketHandle>(::socket(AF_INET, SOCK_STREAM, 0));
  if (m_serverSocket == kInvalidSocket) {
    std::cerr << "[REST] failed to create socket\n";
    return false;
  }
  int opt = 1;
  setsockopt(nativeSocket(m_serverSocket), SOL_SOCKET, SO_REUSEADDR,
             reinterpret_cast<const char *>(&opt), sizeof(opt));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(m_port);
  if (m_bindAddress.empty() ||
      ::inet_pton(AF_INET, m_bindAddress.c_str(), &addr.sin_addr) != 1) {
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  }
  if (::bind(nativeSocket(m_serverSocket), reinterpret_cast<sockaddr *>(&addr),
             sizeof(addr)) < 0) {
    std::cerr << "[REST] failed to bind " << m_bindAddress << ":" << m_port << "\n";
    CLOSESOCKET(nativeSocket(m_serverSocket));
    m_serverSocket = kInvalidSocket;
    return false;
  }
  if (::listen(nativeSocket(m_serverSocket), 8) < 0) {
    std::cerr << "[REST] failed to listen on port " << m_port << "\n";
    CLOSESOCKET(nativeSocket(m_serverSocket));
    m_serverSocket = kInvalidSocket;
    return false;
  }
  m_running.store(true);
  m_acceptThread = std::thread(&RestServer::acceptLoop, this);
  std::cout << "[REST] anonymous control API listening on " << m_bindAddress
            << ":" << m_port << "\n";
  return true;
}

void RestServer::stop() {
  if (!m_running.exchange(false)) return;
  if (m_serverSocket != kInvalidSocket) {
#if defined(_WIN32)
    shutdown(nativeSocket(m_serverSocket), SD_BOTH);
#else
    ::shutdown(nativeSocket(m_serverSocket), SHUT_RDWR);
#endif
    CLOSESOCKET(nativeSocket(m_serverSocket));
    m_serverSocket = kInvalidSocket;
  }
  if (m_acceptThread.joinable()) m_acceptThread.join();
  // Shut the live connections down so a worker blocked in recv() returns
  // immediately, rather than making shutdown wait out the receive timeout.
  {
    std::lock_guard<std::mutex> lock(m_clientSocketsMutex);
    for (SocketHandle sock : m_clientSockets) {
#if defined(_WIN32)
      shutdown(nativeSocket(sock), SD_BOTH);
      CLOSESOCKET(nativeSocket(sock));
#else
      ::shutdown(nativeSocket(sock), SHUT_RDWR);
#endif
    }
#if defined(_WIN32)
    m_clientSockets.clear();
#endif
  }
  // Join the client workers.
  std::vector<std::shared_ptr<ClientThread>> clients;
  {
    std::lock_guard<std::mutex> lock(m_clientThreadsMutex);
    clients.swap(m_clientThreads);
  }
  for (auto &entry : clients) {
    if (entry->thread.joinable()) entry->thread.join();
  }
}

void RestServer::acceptLoop() {
  while (m_running.load()) {
    sockaddr_in clientAddr{};
    SocketLen len = sizeof(clientAddr);
    const SocketHandle client = static_cast<SocketHandle>(
        ::accept(nativeSocket(m_serverSocket),
                 reinterpret_cast<sockaddr *>(&clientAddr), &len));
    if (client == kInvalidSocket) {
      if (!m_running.load()) break;
      continue;
    }
    // Both of these matter for a peer that vanishes mid-exchange: MSG_NOSIGNAL
    // keeps the resulting SIGPIPE from killing the process, and the receive
    // timeout keeps a client that connects but never sends from pinning its
    // own thread.
    configureSendNoSigPipe(client);
    setRecvTimeoutMs(client, kClientRecvTimeoutMs);
    {
      std::lock_guard<std::mutex> lock(m_clientSocketsMutex);
      m_clientSockets.push_back(client);
    }
    // Serve each connection on its own thread. Handling it inline would make
    // a single stalled client deny the API to every other client for the
    // duration of the receive timeout.
    auto entry = std::make_shared<ClientThread>();
    std::thread worker([this, client, entry]() {
      handleConnection(client);
      bool closeClient = false;
      {
        // Remove the socket from the live list while holding the same lock
        // stop() uses. If stop() already closed it to interrupt a Winsock
        // recv, it also removed it and owns the close.
        std::lock_guard<std::mutex> lock(m_clientSocketsMutex);
        const auto it =
            std::find(m_clientSockets.begin(), m_clientSockets.end(), client);
        if (it != m_clientSockets.end()) {
          m_clientSockets.erase(it);
          closeClient = true;
        }
      }
      if (closeClient) {
        CLOSESOCKET(nativeSocket(client));
      }
      entry->done.store(true, std::memory_order_release);
    });
    {
      std::lock_guard<std::mutex> lock(m_clientThreadsMutex);
      m_clientThreads.push_back(entry);
      entry->thread = std::move(worker);
    }
    reapFinishedClientThreads();
  }
}

void RestServer::reapFinishedClientThreads() {
  std::vector<std::shared_ptr<ClientThread>> finished;
  {
    std::lock_guard<std::mutex> lock(m_clientThreadsMutex);
    auto it = m_clientThreads.begin();
    while (it != m_clientThreads.end()) {
      if ((*it)->done.load(std::memory_order_acquire)) {
        finished.push_back(*it);
        it = m_clientThreads.erase(it);
      } else {
        ++it;
      }
    }
  }
  // Join outside the lock: a thread cannot be joined while holding the mutex
  // its worker might still need.
  for (auto &entry : finished) {
    if (entry->thread.joinable()) entry->thread.join();
  }
}

void RestServer::handleConnection(SocketHandle clientSocket) {
  std::string request;
  char buf[2048];
  size_t headerEnd = std::string::npos;
  // Read until end of headers.
  while (request.size() < 64 * 1024) {
    const auto n = ::recv(nativeSocket(clientSocket), buf, sizeof(buf), 0);
    if (n <= 0) break;
    request.append(buf, static_cast<size_t>(n));
    headerEnd = request.find("\r\n\r\n");
    if (headerEnd != std::string::npos) break;
  }
  if (headerEnd == std::string::npos) {
    sendResponse(clientSocket, 400, "Bad Request", "{\"ok\":false}");
    return;
  }

  // Request line.
  const size_t lineEnd = request.find("\r\n");
  const std::string requestLine = request.substr(0, lineEnd);
  std::istringstream rl(requestLine);
  std::string method, target;
  rl >> method >> target;

  if (method == "OPTIONS") {
    sendResponse(clientSocket, 204, "No Content", "");
    return;
  }

  std::string path = target;
  std::string query;
  const size_t q = target.find('?');
  if (q != std::string::npos) {
    path = target.substr(0, q);
    query = target.substr(q + 1);
  }

  // Determine body length and ensure we've read it all.
  size_t contentLength = 0;
  {
    std::string headers = request.substr(0, headerEnd);
    std::transform(headers.begin(), headers.end(), headers.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const size_t cl = headers.find("content-length:");
    if (cl != std::string::npos) {
      contentLength = static_cast<size_t>(std::strtoul(
          request.c_str() + cl + std::strlen("content-length:"), nullptr, 10));
    }
  }
  std::string body = request.substr(headerEnd + 4);
  while (body.size() < contentLength && body.size() < 64 * 1024) {
    const auto n = ::recv(nativeSocket(clientSocket), buf, sizeof(buf), 0);
    if (n <= 0) break;
    body.append(buf, static_cast<size_t>(n));
  }

  // Status endpoint.
  if (path == "/api/status" || path == "/status" ||
      (method == "GET" && (path == "/" || path == "/api"))) {
    // Connections are served concurrently, so serialize the control-plane
    // calls: two clients could otherwise interleave a "set gain" and a "read
    // status" and observe a half-applied state. The lock is taken here, after
    // the request has been read, so a slow client never blocks the others.
    std::lock_guard<std::recursive_mutex> controlsLock(m_controlsMutex);
    const std::string status =
        m_controls.statusJson ? m_controls.statusJson() : "{}";
    sendResponse(clientSocket, 200, "OK", status);
    return;
  }

  // Collect params from query string and body.
  std::vector<std::pair<std::string, std::string>> params;
  if (!query.empty()) parseFormParams(query, params);
  if (!body.empty()) {
    const std::string b = trim(body);
    if (!b.empty() && b[0] == '{') {
      parseFlatJson(b, params);
    } else {
      parseFormParams(b, params);
    }
  }

  int applied = 0;
  std::string responseBody;
  {
    std::lock_guard<std::recursive_mutex> controlsLock(m_controlsMutex);
    responseBody = applyParams(params, applied);
  }
  sendResponse(clientSocket, 200, "OK", responseBody);
  if (m_verboseLogging.load()) {
    std::cout << "[REST] " << method << " " << path << " applied " << applied
              << " setting(s)\n";
  }
}

std::string RestServer::applyParams(
    const std::vector<std::pair<std::string, std::string>> &params,
    int &appliedCount) {
  appliedCount = 0;
  std::ostringstream errs;
  auto note = [&](const std::string &k) {
    if (errs.tellp() > 0) errs << ",";
    errs << "\"" << k << "\"";
  };

  for (const auto &kv : params) {
    const std::string &key = kv.first;
    const std::string &val = kv.second;
    long ival = 0;
    double dval = 0.0;
    bool bval = false;

    if ((key == "freq_hz" || key == "frequency") && toInt(val, ival)) {
      // Range-check before the narrowing cast: a value above UINT32_MAX would
      // silently truncate, and the band check keeps the request inside the
      // FM broadcast band. The setFrequencyHz control re-checks the band.
      if (m_controls.setFrequencyHz && ival > 0 &&
          ival <= static_cast<long>(fm_tuner::kFmBroadcastMaxFreqHz) &&
          m_controls.setFrequencyHz(static_cast<uint32_t>(ival)))
        appliedCount++;
      else
        note(key);
    } else if (key == "freq_khz" && toInt(val, ival)) {
      // Multiply in 64-bit, then band-check, then narrow. Doing the multiply
      // in uint32_t first would wrap (e.g. 5000000 kHz -> 705032704 Hz) and
      // land the tuner on a plausible-looking but wrong frequency.
      const long long hz = static_cast<long long>(ival) * 1000LL;
      if (m_controls.setFrequencyHz && ival > 0 && hz > 0 &&
          hz <= static_cast<long long>(fm_tuner::kFmBroadcastMaxFreqHz) &&
          m_controls.setFrequencyHz(static_cast<uint32_t>(hz)))
        appliedCount++;
      else
        note(key);
    } else if (key == "gain_db" && toDouble(val, dval)) {
      if (m_controls.setGainDb && m_controls.setGainDb(dval))
        appliedCount++;
      else
        note(key);
    } else if ((key == "agc" || key == "auto_gain") && toBool(val, bval)) {
      if (m_controls.setAutoGain && m_controls.setAutoGain(bval))
        appliedCount++;
      else
        note(key);
    } else if (key == "bandwidth_hz" && toInt(val, ival)) {
      if (m_controls.setBandwidthHz &&
          m_controls.setBandwidthHz(clampBandwidthHz(ival)))
        appliedCount++;
      else
        note(key);
    } else if (key == "bandwidth_khz" && toInt(val, ival)) {
      // Multiply in 64-bit, then clamp, then narrow: doing the multiply in
      // int is signed-integer overflow for a large value.
      const long long hz = static_cast<long long>(ival) * 1000LL;
      if (m_controls.setBandwidthHz &&
          m_controls.setBandwidthHz(clampBandwidthHz(hz)))
        appliedCount++;
      else
        note(key);
    } else if (key == "lna" && toInt(val, ival)) {
      if (m_controls.setLnaState && m_controls.setLnaState(static_cast<int>(ival)))
        appliedCount++;
      else
        note(key);
    } else if (key == "antenna" && toInt(val, ival)) {
      if (m_controls.setAntenna && m_controls.setAntenna(static_cast<int>(ival)))
        appliedCount++;
      else
        note(key);
    } else if (key == "bias_tee" && toBool(val, bval)) {
      if (m_controls.setBiasTee && m_controls.setBiasTee(bval))
        appliedCount++;
      else
        note(key);
    } else if (key == "ppm" && toInt(val, ival)) {
      if (m_controls.setPpm && m_controls.setPpm(static_cast<int>(ival)))
        appliedCount++;
      else
        note(key);
    } else if (key == "rtl_agc" && toBool(val, bval)) {
      if (m_controls.setRtlAgc && m_controls.setRtlAgc(bval))
        appliedCount++;
      else
        note(key);
    } else if (key == "deemphasis" && toInt(val, ival)) {
      if (m_controls.setDeemphasis && m_controls.setDeemphasis(static_cast<int>(ival)))
        appliedCount++;
      else
        note(key);
    } else if (key == "blend" && toInt(val, ival)) {
      if (m_controls.setBlendMode && m_controls.setBlendMode(static_cast<int>(ival)))
        appliedCount++;
      else
        note(key);
    } else if (key == "force_mono" && toBool(val, bval)) {
      if (m_controls.setForceMono && m_controls.setForceMono(bval))
        appliedCount++;
      else
        note(key);
    } else if (key == "volume" && toInt(val, ival)) {
      if (m_controls.setVolume && m_controls.setVolume(static_cast<int>(ival)))
        appliedCount++;
      else
        note(key);
    } else if (key == "action") {
      if (val == "start" && m_controls.start && m_controls.start())
        appliedCount++;
      else if (val == "stop" && m_controls.stop && m_controls.stop())
        appliedCount++;
      else if (val == "reset_stats" && m_controls.resetStats &&
               m_controls.resetStats())
        appliedCount++;
      else
        note(key);
    } else if (!key.empty()) {
      note(key);
    }
  }

  std::ostringstream oss;
  oss << "{\"ok\":" << (errs.tellp() == 0 ? "true" : "false")
      << ",\"applied\":" << appliedCount;
  if (errs.tellp() > 0) {
    oss << ",\"rejected\":[" << errs.str() << "]";
  }
  oss << ",\"status\":"
      << (m_controls.statusJson ? m_controls.statusJson() : std::string("{}"))
      << "}";
  return oss.str();
}
