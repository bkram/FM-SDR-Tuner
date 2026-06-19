#include "rest_server.h"

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
#include <unistd.h>
using SocketLen = socklen_t;
#define CLOSESOCKET ::close
#endif

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>

namespace {

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

void sendResponse(int sock, int status, const std::string &statusText,
                  const std::string &body) {
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
    const auto n = ::send(sock, out.data() + sent, out.size() - sent, 0);
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
  // on the XDR server (or anyone else) having called WSAStartup first.
  // Refcounted, so a redundant call alongside XDRServer's is harmless.
  {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
      std::cerr << "[REST] WSAStartup failed\n";
      return false;
    }
  }
#endif
  m_serverSocket = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
  if (m_serverSocket < 0) {
    std::cerr << "[REST] failed to create socket\n";
    return false;
  }
  int opt = 1;
  setsockopt(m_serverSocket, SOL_SOCKET, SO_REUSEADDR,
             reinterpret_cast<const char *>(&opt), sizeof(opt));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(m_port);
  if (m_bindAddress.empty() ||
      ::inet_pton(AF_INET, m_bindAddress.c_str(), &addr.sin_addr) != 1) {
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  }
  if (::bind(m_serverSocket, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    std::cerr << "[REST] failed to bind " << m_bindAddress << ":" << m_port << "\n";
    CLOSESOCKET(m_serverSocket);
    m_serverSocket = -1;
    return false;
  }
  if (::listen(m_serverSocket, 8) < 0) {
    std::cerr << "[REST] failed to listen on port " << m_port << "\n";
    CLOSESOCKET(m_serverSocket);
    m_serverSocket = -1;
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
  if (m_serverSocket >= 0) {
#if defined(_WIN32)
    shutdown(m_serverSocket, SD_BOTH);
#else
    ::shutdown(m_serverSocket, SHUT_RDWR);
#endif
    CLOSESOCKET(m_serverSocket);
    m_serverSocket = -1;
  }
  if (m_acceptThread.joinable()) m_acceptThread.join();
}

void RestServer::acceptLoop() {
  while (m_running.load()) {
    sockaddr_in clientAddr{};
    SocketLen len = sizeof(clientAddr);
    const int client = static_cast<int>(
        ::accept(m_serverSocket, reinterpret_cast<sockaddr *>(&clientAddr), &len));
    if (client < 0) {
      if (!m_running.load()) break;
      continue;
    }
    handleConnection(client);
    CLOSESOCKET(client);
  }
}

void RestServer::handleConnection(int clientSocket) {
  std::string request;
  char buf[2048];
  size_t headerEnd = std::string::npos;
  // Read until end of headers.
  while (request.size() < 64 * 1024) {
    const auto n = ::recv(clientSocket, buf, sizeof(buf), 0);
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
    const auto n = ::recv(clientSocket, buf, sizeof(buf), 0);
    if (n <= 0) break;
    body.append(buf, static_cast<size_t>(n));
  }

  // Status endpoint.
  if (path == "/api/status" || path == "/status" ||
      (method == "GET" && (path == "/" || path == "/api"))) {
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
  const std::string responseBody = applyParams(params, applied);
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
      if (m_controls.setFrequencyHz && ival > 0 &&
          m_controls.setFrequencyHz(static_cast<uint32_t>(ival)))
        appliedCount++;
      else
        note(key);
    } else if (key == "freq_khz" && toInt(val, ival)) {
      if (m_controls.setFrequencyHz && ival > 0 &&
          m_controls.setFrequencyHz(static_cast<uint32_t>(ival) * 1000U))
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
      if (m_controls.setBandwidthHz && m_controls.setBandwidthHz(static_cast<int>(ival)))
        appliedCount++;
      else
        note(key);
    } else if (key == "bandwidth_khz" && toInt(val, ival)) {
      if (m_controls.setBandwidthHz &&
          m_controls.setBandwidthHz(static_cast<int>(ival) * 1000))
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
