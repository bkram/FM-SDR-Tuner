#include "xdr_server.h"

#include "tuning_limits.h"
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif
#include "socket_subsystem.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <random>
#include <sstream>

namespace {
#if defined(_WIN32)
using SocketLen = int;
#else
using SocketLen = socklen_t;
#endif

constexpr size_t kMaxClientCommandLen = 1024;

// The widest channel bandwidth the tuner accepts from a client. The
// signal-meter estimator analyses the channel with an FFT whose length is
// capped at 8192 points; beyond roughly the sample rate the side-band window
// wraps back over the channel and the meter measures the signal as its own
// noise floor. The tuner runs at 256 kHz, so a bandwidth at or above the
// sample rate is never a meaningful request.
constexpr int kMaxChannelBandwidthHz = 256000;

// The XDR protocol carries frequencies in kHz: xdr-gtk sends "T101700" for
// 101.7 MHz and research/xdrd/xdrd.c stores it verbatim (atoi) as kHz. Some
// non-standard clients send Hz instead, so a 'T' argument at or above this
// value is taken as Hz. The FM broadcast band starts at 64 MHz = 64000 kHz,
// so the crossover sits far below any legitimate FM frequency.
constexpr unsigned long long kHzToHzCrossover = 1000000ULL;

// Protocol detection: how long to keep waiting for the client's first byte,
// and how long each individual peek waits. The FM-DX client speaks first (it
// sends its "x" handshake immediately on connect); the XDR client waits for
// the server's salt and sends nothing, so a client that has not spoken yet is
// assumed to be an XDR client. The window therefore has to be long enough
// that a slow FM-DX client is still recognised, and short enough that it
// fits inside the reference client's own salt timeout
// (research/xdr-gtk/src/tuner-conn.h: SOCKET_AUTH_TIMEOUT == 5 s).
constexpr int kProtocolDetectTimeoutMs = 1000;
constexpr int kProtocolDetectPollMs = 50;

// The FM-DX handshake line and the XDR auth hash must both arrive promptly;
// without a deadline a stalled client pins its thread forever.
constexpr int kHandshakeTimeoutMs = 5000;

uint64_t steadyNowMs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

void setRecvTimeoutMs(SocketHandle clientSocket, int timeoutMs) {
#if defined(_WIN32)
  const DWORD timeout = timeoutMs > 0 ? static_cast<DWORD>(timeoutMs) : 0;
  setsockopt(static_cast<SOCKET>(clientSocket), SOL_SOCKET, SO_RCVTIMEO,
             reinterpret_cast<const char *>(&timeout), sizeof(timeout));
#else
  struct timeval tv;
  tv.tv_sec = timeoutMs / 1000;
  tv.tv_usec = (timeoutMs % 1000) * 1000;
  setsockopt(static_cast<int>(clientSocket), SOL_SOCKET, SO_RCVTIMEO, &tv,
             sizeof(tv));
#endif
}

void configureSendNoSigPipe(SocketHandle sock) {
#if defined(SO_NOSIGPIPE)
  const int opt = 1;
  setsockopt(static_cast<int>(sock), SOL_SOCKET, SO_NOSIGPIPE, &opt,
             sizeof(opt));
#else
  (void)sock;
#endif
}

int sendSocket(SocketHandle sock, const char *data, size_t len) {
#if defined(_WIN32)
  return send(static_cast<SOCKET>(sock), data, static_cast<int>(len), 0);
#elif defined(MSG_NOSIGNAL)
  return static_cast<int>(
      send(static_cast<int>(sock), data, len, MSG_NOSIGNAL));
#else
  return static_cast<int>(send(static_cast<int>(sock), data, len, 0));
#endif
}

void closeSocket(SocketHandle sock) {
#if defined(_WIN32)
  closesocket(static_cast<SOCKET>(sock));
#else
  close(static_cast<int>(sock));
#endif
}

void shutdownSocket(SocketHandle sock) {
#if defined(_WIN32)
  shutdown(static_cast<SOCKET>(sock), SD_BOTH);
#else
  shutdown(static_cast<int>(sock), SHUT_RDWR);
#endif
}

// Wait until the socket has data (or the timeout elapses). Used to avoid
// blocking in accept(): shutdown() on a listening socket does not reliably
// wake a blocked accept() on macOS/BSD, so a poll-based wait is what lets
// the accept loop notice the shutdown request.
bool waitForReadable(SocketHandle sock, int timeoutMs) {
#if defined(_WIN32)
  fd_set readSet;
  FD_ZERO(&readSet);
  FD_SET(static_cast<SOCKET>(sock), &readSet);
  timeval tv;
  tv.tv_sec = timeoutMs / 1000;
  tv.tv_usec = (timeoutMs % 1000) * 1000;
  const int ready =
      select(0, &readSet, nullptr, nullptr, &tv); // ignore fd on Windows
  return ready > 0;
#else
  struct pollfd pfd;
  pfd.fd = static_cast<int>(sock);
  pfd.events = POLLIN;
  pfd.revents = 0;
  const int ready = poll(&pfd, 1, timeoutMs);
  return ready > 0;
#endif
}

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

int lastSocketError() {
#if defined(_WIN32)
  return WSAGetLastError();
#else
  return errno;
#endif
}

bool socketInterrupted(int err) {
#if defined(_WIN32)
  return err == WSAEINTR;
#else
  return err == EINTR;
#endif
}

bool socketWouldBlock(int err) {
#if defined(_WIN32)
  return err == WSAEWOULDBLOCK || err == WSAETIMEDOUT;
#else
  return err == EAGAIN || err == EWOULDBLOCK;
#endif
}

bool recvLine(SocketHandle clientSocket, std::string &line, size_t maxLen) {
  line.clear();

  while (line.length() < maxLen) {
    char ch = '\0';
    const auto n = recv(nativeSocket(clientSocket), &ch, 1, 0);
    if (n <= 0) {
      return false;
    }

    if (ch == '\n') {
      return true;
    }

    if (ch != '\r') {
      line.push_back(ch);
    }
  }

  line.clear();
  return false;
}

// The single place the protocol's command-length limit is expressed. Both
// the accumulated-buffer pre-check and the per-line check call this, so the
// effective limit cannot drift between the two.
bool lineExceedsLimit(size_t length) { return length > kMaxClientCommandLen; }

bool commandBufferOverflowed(const std::string &commandBuffer) {
  const size_t newlinePos = commandBuffer.find('\n');
  if (newlinePos != std::string::npos) {
    return lineExceedsLimit(newlinePos);
  }
  return lineExceedsLimit(commandBuffer.size());
}

bool parseIntValue(const std::string &arg, int &out) {
  if (arg.empty()) {
    return false;
  }
  try {
    size_t parsed = 0;
    int value = std::stoi(arg, &parsed);
    if (parsed != arg.size()) {
      return false;
    }
    out = value;
    return true;
  } catch (...) {
    return false;
  }
}

bool parseTefCustomValue(const std::string &arg, int &out) {
  if (arg.size() != 2) {
    return false;
  }
  if ((arg[0] != '0' && arg[0] != '1') || (arg[1] != '0' && arg[1] != '1')) {
    return false;
  }
  out = (arg[0] - '0') * 10 + (arg[1] - '0');
  return true;
}

bool parseFrequencyHz(const std::string &arg, uint32_t &outHz) {
  if (arg.empty()) {
    return false;
  }
  try {
    size_t parsed = 0;
    unsigned long long raw = std::stoull(arg, &parsed);
    if (parsed != arg.size() || raw == 0) {
      return false;
    }
    // The XDR protocol is kHz-based: xdr-gtk sends "T101700" for 101.7 MHz
    // and research/xdrd/xdrd.c stores it verbatim (atoi) as kHz. Some
    // non-standard clients send Hz instead, so a value at or above 1 MHz is
    // taken as Hz and anything below as kHz. The FM broadcast band starts at
    // 64 MHz, so no legitimate kHz value is ever near the 1 MHz crossover.
    if (raw < kHzToHzCrossover) {
      raw *= 1000ULL;
    }
    if (raw > 0xFFFFFFFFULL) {
      return false;
    }
    outHz = static_cast<uint32_t>(raw);
    return true;
  } catch (...) {
    return false;
  }
}

std::string formatGain(int gain) {
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "G%02d", gain);
  return std::string(buffer);
}

std::string formatSampling(int interval, int detector) {
  std::ostringstream oss;
  oss << "I" << interval << "," << detector;
  return oss.str();
}

uint8_t evaluatePiState(const std::array<uint16_t, 64> &piBuffer,
                        const std::array<uint8_t, 8> &piErrorBuffer,
                        uint8_t fill, uint16_t value) {
  uint8_t count = 0;
  uint8_t correctCount = 0;

  for (uint8_t i = 0; i < fill; i++) {
    if (piBuffer[i] == value) {
      count++;
      if ((piErrorBuffer[i / 8] & (1 << (i % 8))) == 0) {
        correctCount++;
      }
    }
  }

  if (correctCount >= 2)
    return 0; // STATE_CORRECT
  if (count >= 2 && correctCount)
    return 1; // STATE_VERY_LIKELY
  if (count >= 3)
    return 2; // STATE_LIKELY
  if (count == 2 || correctCount)
    return 3; // STATE_UNLIKELY
  return 4;   // STATE_INVALID
}

bool ensureSocketSubsystem() {
#if defined(_WIN32)
  // One reference for the lifetime of the process; the subsystem is shut
  // down when the static lease is destroyed at exit.
  static SocketSubsystemLease lease;
  return lease.ok();
#else
  return true;
#endif
}
} // namespace

void XDRServer::addClientSocket(SocketHandle clientSocket) {
  std::lock_guard<std::mutex> lock(m_clientSocketsMutex);
  m_clientSockets.push_back(clientSocket);
}

void XDRServer::removeClientSocket(SocketHandle clientSocket) {
  std::lock_guard<std::mutex> lock(m_clientSocketsMutex);
  auto it =
      std::find(m_clientSockets.begin(), m_clientSockets.end(), clientSocket);
  if (it != m_clientSockets.end()) {
    m_clientSockets.erase(it);
  }
}

void XDRServer::joinClientThreads() {
  std::vector<std::shared_ptr<ClientThread>> entries;
  {
    std::lock_guard<std::mutex> lock(m_clientThreadsMutex);
    entries.swap(m_clientThreads);
  }
  for (const auto &entry : entries) {
    if (entry->thread.joinable()) {
      entry->thread.join();
    }
  }
}

XDRServer::XDRServer(uint16_t port)
    : m_port(port), m_serverSocket(kInvalidSocket), m_running(false),
      m_guestMode(false), m_verboseLogging(true), m_mode(0),
      m_frequency(88500000), m_volume(100), m_deemphasis(0), m_filter(-1),
      m_bandwidth(0), m_antenna(0), m_gain(0), m_agcMode(2), m_daa(0),
      m_squelch(0), m_rotator(0), m_samplingInterval(66), m_detector(0),
      m_forceMono(false), m_signalDeci(0), m_signalStereo(false),
      m_signalForcedMono(false), m_cci(-1), m_aci(-1), m_pilotTenthsKHz(0) {
  m_scanStartKHz = 87500;
  m_scanStopKHz = 108000;
  m_scanStepKHz = 100;
  m_scanBandwidthHz = 0;
  m_scanAntenna = 0;
  m_scanContinuous = false;
  m_scanStartPending = false;
  m_scanCancelPending = false;
  m_piFill = 0;
  m_piPos = 63;
  m_piBuffer.fill(0);
  m_piErrorBuffer.fill(0);
  m_piLastState = 4;
  m_piLastValue = 0xFFFF;
  m_lastRdsMs = 0;
}

XDRServer::~XDRServer() { stop(); }

void XDRServer::setPassword(const std::string &password) {
  m_password = password;
}

void XDRServer::setGuestMode(bool enabled) { m_guestMode = enabled; }

void XDRServer::setVerboseLogging(bool enabled) { m_verboseLogging = enabled; }

std::string XDRServer::generateSalt() {
  static const char chars[] =
      "QWERTYUIOPASDFGHJKLZXCVBNMqwertyuiopasdfghjklzxcvbnm0123456789_-";
  constexpr size_t len = sizeof(chars) - 1;
  unsigned char random_data[SALT_LENGTH];

  if (!RAND_bytes(random_data, sizeof(random_data))) {
    // Fail closed: a predictable salt would undermine the challenge/response,
    // so refuse to authenticate this session rather than fall back to a
    // non-cryptographic PRNG.
    if (m_verboseLogging) {
      std::cerr << "[XDR] RAND_bytes failed; refusing to issue a salt\n";
    }
    return {};
  }

  std::string salt;
  for (int i = 0; i < SALT_LENGTH; i++) {
    salt += chars[static_cast<size_t>(random_data[i]) % len];
  }
  return salt;
}

std::string XDRServer::computeSHA1(const std::string &salt,
                                   const std::string &password) {
  unsigned char sha[EVP_MAX_MD_SIZE];
  unsigned int shaLen = 0;
  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  if (ctx == nullptr) {
    return {};
  }

  if (EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr) != 1) {
    EVP_MD_CTX_free(ctx);
    return {};
  }
  if (EVP_DigestUpdate(ctx, salt.data(), salt.size()) != 1) {
    EVP_MD_CTX_free(ctx);
    return {};
  }
  if (EVP_DigestUpdate(ctx, password.data(), password.size()) != 1) {
    EVP_MD_CTX_free(ctx);
    return {};
  }
  if (EVP_DigestFinal_ex(ctx, sha, &shaLen) != 1) {
    EVP_MD_CTX_free(ctx);
    return {};
  }
  EVP_MD_CTX_free(ctx);

  if (shaLen == 0) {
    return {};
  }

  char sha_string[EVP_MAX_MD_SIZE * 2 + 1];
  for (unsigned int i = 0; i < shaLen; i++) {
    std::snprintf(sha_string + (i * 2), 3, "%02x", sha[i]);
  }
  sha_string[shaLen * 2] = '\0';
  return std::string(sha_string);
}

bool XDRServer::authenticate(const std::string &salt,
                             const std::string &passwordHash) {
  std::string expected = computeSHA1(salt, m_password);
  if (expected.empty() || expected.length() != passwordHash.length()) {
    return false;
  }
  // Constant-time comparison so response timing can't be used to recover the
  // expected hash byte-by-byte. Normalize hex case first (clients may send
  // upper- or lower-case), then CRYPTO_memcmp over the equal-length buffers.
  auto toLowerHex = [](std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    return s;
  };
  const std::string a = toLowerHex(expected);
  const std::string b = toLowerHex(passwordHash);
  return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

std::string XDRServer::buildXdrStateSnapshot() const {
  std::ostringstream oss;
  oss << "M" << m_mode << "\n"
      << "Y" << m_volume << "\n"
      << "T" << (m_frequency / 1000) << "\n"
      << "D" << m_deemphasis << "\n"
      << "A" << m_agcMode << "\n"
      << "W" << m_bandwidth << "\n";

  oss << "Z" << m_antenna << "\n"
      << formatGain(m_gain) << "\n"
      << "V" << m_daa << "\n"
      << "Q" << m_squelch << "\n"
      << "C" << m_rotator << "\n"
      << formatSampling(m_samplingInterval, m_detector);
  return oss.str();
}

std::string XDRServer::buildSignalLine() const {
  const bool forcedMono = m_signalForcedMono.load();
  const bool stereo = m_signalStereo.load();
  const int cci = m_cci.load();
  const int aci = m_aci.load();
  const double level = m_signalDeci.load() / 10.0;

  char mode = 'm';
  if (forcedMono) {
    mode = stereo ? 'S' : 'M';
  } else if (stereo) {
    mode = 's';
  }

  std::ostringstream oss;
  oss << "S" << mode << std::fixed << std::setprecision(1) << level << ","
      << cci << "," << aci;
  return oss.str();
}

void XDRServer::updateSignal(float level, bool stereo, bool forcedMono, int cci,
                             int aci) {
  const int deci =
      static_cast<int>(std::round(std::clamp(level, 0.0f, 120.0f) * 10.0f));
  m_signalDeci = deci;
  m_signalStereo = stereo;
  m_signalForcedMono = forcedMono;
  m_cci = cci;
  m_aci = aci;
}

void XDRServer::updatePilot(int pilotTenthsKHz) {
  m_pilotTenthsKHz = std::clamp(pilotTenthsKHz, 0, 750);
}

void XDRServer::updateRDS(uint16_t blockA, uint16_t blockB, uint16_t blockC,
                          uint16_t blockD, uint8_t errors) {
  m_lastRdsMs = steadyNowMs();

  std::lock_guard<std::mutex> lock(m_rdsMutex);

  constexpr size_t kMaxRdsQueue = 256;

  const uint8_t blockAErr = static_cast<uint8_t>((errors >> 6) & 0x03u);
  const uint8_t blockBErr = static_cast<uint8_t>((errors >> 4) & 0x03u);

  m_piPos = (m_piPos + 1) % 64;
  m_piBuffer[m_piPos] = blockA;

  const uint8_t errPos = m_piPos / 8;
  const uint8_t errBit = m_piPos % 8;
  if (blockAErr != 0) {
    m_piErrorBuffer[errPos] |= (1 << errBit);
  } else {
    m_piErrorBuffer[errPos] &= ~(1 << errBit);
  }

  if (m_piFill < 64) {
    m_piFill++;
  }

  const uint8_t piState =
      evaluatePiState(m_piBuffer, m_piErrorBuffer, m_piFill, blockA);
  const bool blockAPresent = (blockAErr != 3);
  const bool piDebounced = blockAPresent && (piState <= 1);

  if (piDebounced) {
    char piBuffer[16];
    std::snprintf(piBuffer, sizeof(piBuffer), "P%04X%.*s", blockA,
                  static_cast<int>(std::min<uint8_t>(blockAErr, 3)), "???");
    if (m_rdsQueue.size() >= kMaxRdsQueue) {
      m_rdsQueue.pop_front();
    }
    m_rdsQueue.emplace_back(m_rdsNextSeq++, std::string(piBuffer));
    m_piLastValue = blockA;
  }

  // PTY/TP/TA/MS bits are in block B. Drop groups where block B had errors to
  // avoid noisy flag flapping in clients.
  if (blockBErr == 0) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "R%04X%04X%04X%02X", blockB, blockC,
                  blockD, errors);
    if (m_rdsQueue.size() >= kMaxRdsQueue) {
      m_rdsQueue.pop_front();
    }
    m_rdsQueue.emplace_back(m_rdsNextSeq++, std::string(buffer));
  }
  m_piLastState = piState;
}

void XDRServer::resetPiHistory() {
  std::lock_guard<std::mutex> lock(m_rdsMutex);
  m_piFill = 0;
  m_piPos = 63;
  m_piBuffer.fill(0);
  m_piErrorBuffer.fill(0);
  m_piLastState = 4;
  m_piLastValue = 0xFFFF;
}

void XDRServer::addOnlineClient(bool authenticated) {
  if (authenticated) {
    m_onlineAuth.fetch_add(1, std::memory_order_relaxed);
  } else {
    m_onlineGuest.fetch_add(1, std::memory_order_relaxed);
  }
}

void XDRServer::removeOnlineClient(bool authenticated) {
  if (authenticated) {
    m_onlineAuth.fetch_sub(1, std::memory_order_relaxed);
  } else {
    m_onlineGuest.fetch_sub(1, std::memory_order_relaxed);
  }
}

std::string XDRServer::onlineLine() const {
  return "o" + std::to_string(m_onlineAuth.load(std::memory_order_relaxed)) +
         "," + std::to_string(m_onlineGuest.load(std::memory_order_relaxed));
}

void XDRServer::broadcastOnlineLine() {
  // The reference daemon pushes the updated count to every client when a
  // peer connects or disconnects (research/xdrd/xdrd.c:561, :597). A client
  // that only reads the line on connect would otherwise never learn that
  // somebody else joined or left.
  std::string line = onlineLine() + "\n";
  std::vector<SocketHandle> sockets;
  {
    std::lock_guard<std::mutex> lock(m_clientSocketsMutex);
    sockets = m_clientSockets;
  }
  for (SocketHandle sock : sockets) {
    sendSocket(sock, line.c_str(), line.length());
  }
}

void XDRServer::setFrequencyState(uint32_t freqHz) {
  if (freqHz == 0) {
    return;
  }
  // Compare against the last frequency this function was told about, not
  // against m_frequency: the 'T' command handler publishes m_frequency itself
  // before this runs, so comparing to m_frequency would never see a change.
  if (freqHz != m_lastAppliedFrequency) {
    m_lastAppliedFrequency = freqHz;
    resetPiHistory();
  }
  m_frequency = freqHz;
}

bool XDRServer::consumeScanStart(ScanConfig &config) {
  if (!m_scanStartPending.exchange(false)) {
    return false;
  }
  config.startKHz = m_scanStartKHz.load();
  config.stopKHz = m_scanStopKHz.load();
  config.stepKHz = m_scanStepKHz.load();
  config.bandwidthHz = m_scanBandwidthHz.load();
  config.antenna = m_scanAntenna.load();
  config.continuous = m_scanContinuous.load();
  return true;
}

bool XDRServer::consumeScanCancel() {
  return m_scanCancelPending.exchange(false);
}

void XDRServer::pushScanLine(const std::string &line) {
  if (line.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(m_scanMutex);
  // The scan publishes one line per retune so the client's plot fills in
  // while the sweep runs. A full 87.5-108 MHz sweep produces 43 lines at a
  // 500 kHz step and 138 at the 30 kHz step the grid cap settles on, so the
  // queue has to hold more than a handful of lines or the client would only
  // ever see the last few. The queue is bounded so a client that never reads
  // cannot make the server grow without limit; the bound is generous enough
  // to cover a full sweep.
  constexpr size_t kMaxQueuedScanLines = 256;
  if (m_scanQueue.size() >= kMaxQueuedScanLines) {
    m_scanQueue.pop_front();
  }
  m_scanQueue.emplace_back(m_scanNextSeq++, "U" + line);
}

bool XDRServer::start() {
  if (m_running) {
    return false;
  }

  if (!ensureSocketSubsystem()) {
    std::cerr << "[XDR] failed to initialize socket subsystem" << "\n";
    return false;
  }

  const SocketHandle serverSocket =
      static_cast<SocketHandle>(socket(AF_INET, SOCK_STREAM, 0));
  if (serverSocket == kInvalidSocket) {
    std::cerr << "[XDR] failed to create server socket (err=" << lastSocketError()
              << ")" << "\n";
    return false;
  }
  m_serverSocket = serverSocket;

  int opt = 1;
#if defined(_WIN32)
  setsockopt(static_cast<SOCKET>(serverSocket), SOL_SOCKET, SO_REUSEADDR,
             reinterpret_cast<const char *>(&opt),
             static_cast<int>(sizeof(opt)));
#else
  setsockopt(static_cast<int>(serverSocket), SOL_SOCKET, SO_REUSEADDR, &opt,
             sizeof(opt));
#endif

  struct sockaddr_in serverAddr;
  memset(&serverAddr, 0, sizeof(serverAddr));
  serverAddr.sin_family = AF_INET;
  serverAddr.sin_addr.s_addr = INADDR_ANY;
  serverAddr.sin_port = htons(m_port);

  if (bind(nativeSocket(serverSocket), (struct sockaddr *)&serverAddr,
           sizeof(serverAddr)) < 0) {
    std::cerr << "[XDR] failed to bind to port " << m_port
              << " (err=" << lastSocketError() << ")" << "\n";
    closeSocket(serverSocket);
    m_serverSocket = kInvalidSocket;
    return false;
  }

  if (listen(nativeSocket(serverSocket), 5) < 0) {
    std::cerr << "[XDR] failed to listen on port " << m_port
              << " (err=" << lastSocketError() << ")" << "\n";
    closeSocket(serverSocket);
    m_serverSocket = kInvalidSocket;
    return false;
  }

  m_running = true;
  m_acceptThread = std::thread([this]() {
    while (m_running) {
      const SocketHandle serverSocket = m_serverSocket.load();
      if (serverSocket == kInvalidSocket) {
        break;
      }
      // Wait for a connection with poll() rather than blocking in accept():
      // shutdown() on a *listening* socket does not reliably wake a blocked
      // accept() (notably on macOS/BSD), so a blocking accept() would hang
      // stop() forever. poll() returns on the timeout, which lets the loop
      // re-check m_running and exit promptly.
      if (!waitForReadable(serverSocket, 200)) {
        continue;
      }

      struct sockaddr_in clientAddr;
      SocketLen clientLen = sizeof(clientAddr);
      const SocketHandle clientSocket = static_cast<SocketHandle>(
          accept(nativeSocket(serverSocket), (struct sockaddr *)&clientAddr,
                 &clientLen));

      if (clientSocket != kInvalidSocket) {
        addClientSocket(clientSocket);
        auto entry = std::make_shared<ClientThread>();
        std::thread worker([this, clientSocket, entry]() {
          handleClient(clientSocket);
          removeClientSocket(clientSocket);
          closeSocket(clientSocket);
          entry->done.store(true, std::memory_order_release);
        });
        {
          std::lock_guard<std::mutex> lock(m_clientThreadsMutex);
          entry->thread = std::move(worker);
          m_clientThreads.push_back(entry);
        }
        // Reap the threads of clients that have already disconnected. A
        // std::thread whose function returned but which was never joined still
        // holds its stack, so leaving them in the vector leaks one stack per
        // connection for the lifetime of the process.
        reapFinishedClientThreads();
      } else if (m_running && socketInterrupted(lastSocketError())) {
        continue;
      }
    }
    // The accept thread owns the listening socket's close: stop() only
    // shutdown()s it, so the fd can never be closed and reused underneath a
    // concurrent accept() call.
    const SocketHandle listeningSocket = m_serverSocket.exchange(kInvalidSocket);
    if (listeningSocket != kInvalidSocket) {
      closeSocket(listeningSocket);
    }
  });

  if (m_verboseLogging.load()) {
    std::cout << "[XDR] listening on port " << m_port << "\n";
  }
  return true;
}

void XDRServer::reapFinishedClientThreads() {
  // Collect the finished threads under the lock, then join them outside it:
  // join() can block, and holding the lock would stall the accept loop.
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
  for (const auto &entry : finished) {
    if (entry->thread.joinable()) {
      entry->thread.join();
    }
  }
}

void XDRServer::stop() {
  if (!m_running) {
    return;
  }

  m_running = false;

  // shutdown() only — the accept thread performs the close(), so the fd is
  // never closed (and potentially reused) while accept() is in flight. The
  // accept loop waits with poll(), so it notices m_running going false and
  // returns within one poll interval.
  const SocketHandle serverSocket = m_serverSocket.load();
  if (serverSocket != kInvalidSocket) {
    shutdownSocket(serverSocket);
  }

  std::vector<SocketHandle> clients;
  {
    std::lock_guard<std::mutex> lock(m_clientSocketsMutex);
    clients = m_clientSockets;
  }
  for (SocketHandle sock : clients) {
    shutdownSocket(sock);
  }

  if (m_acceptThread.joinable()) {
    m_acceptThread.join();
  }
  joinClientThreads();
}

void XDRServer::handleClient(SocketHandle clientSocket) {
  configureSendNoSigPipe(clientSocket);
  char clientIP[INET_ADDRSTRLEN];
  struct sockaddr_in clientAddr;
  SocketLen clientLen = sizeof(clientAddr);
  getpeername(nativeSocket(clientSocket), (struct sockaddr *)&clientAddr,
               &clientLen);
  inet_ntop(AF_INET, &clientAddr.sin_addr, clientIP, INET_ADDRSTRLEN);

  std::cout << "[XDR] client connected from " << clientIP << "\n";

  // Protocol detection: the FM-DX client opens with an "x" handshake line,
  // the XDR client waits for the server's salt. Peek with a generous timeout
  // and keep waiting while nothing has arrived, rather than falling through
  // to the XDR path on the first timeout: a client that is merely slow to
  // send its first byte would otherwise be misclassified as an XDR client
  // and then handed a salt it does not understand.
  const auto detectDeadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(
                                              kProtocolDetectTimeoutMs);
  setRecvTimeoutMs(clientSocket, kProtocolDetectPollMs);
  char firstByte = '\0';
  ssize_t n = 0;
  while (m_running) {
    n = recv(nativeSocket(clientSocket), &firstByte, 1, MSG_PEEK);
    if (n > 0) {
      break;
    }
    if (n == 0) {
      break; // peer closed before saying anything
    }
    if (!socketWouldBlock(lastSocketError())) {
      break; // hard error
    }
    // Timed out with nothing to read: keep waiting for the client's first
    // byte, but bound the total wait so a silent peer cannot pin the thread.
    if (std::chrono::steady_clock::now() >= detectDeadline) {
      break;
    }
  }
  setRecvTimeoutMs(clientSocket, 0);

  const bool isFmdxProtocol = (n > 0 && firstByte == 'x');

  if (isFmdxProtocol) {
    if (m_verboseLogging.load()) {
      std::cout << "[XDR] detected FM-DX protocol handshake" << "\n";
    }
    // The "x" startup line is *not* consumed here. The reference FM-DX
    // client sends it exactly once, in its startup handshake, and expects
    // the server to answer "\nOK\n" before it enters its command loop
    // (research/FM-DX-Tuner/src/Controllers/Dispatcher/Dispatcher.cpp:66
    // — Dispatcher::await() reads until it sees FMDX_TUNER_PROTOCOL_STARTUP
    // and then returns; the reply comes from cbStartup). Consuming the "x"
    // here and replying "1" meant the command loop never saw the startup
    // line, so tunerStarted stayed false and every subsequent command was
    // answered "ER" — the client could never start or tune the radio.
    handleFmdxClient(clientSocket, clientIP);
  } else {
    if (m_verboseLogging.load()) {
      std::cout << "[XDR] detected XDR protocol handshake" << "\n";
    }
    handleXdrClient(clientSocket, clientIP);
  }

  std::cout << "[XDR] client disconnected from " << clientIP << "\n";
}

void XDRServer::handleFmdxClient(SocketHandle clientSocket,
                               const char *clientIP) {
  bool tunerStarted = false;
  bool disconnectRequested = false;
  char cmdBuffer[256];
  std::string command;
  setRecvTimeoutMs(clientSocket, 200);

  // The FM-DX protocol has no challenge/response of its own, but the tuner
  // still has to be protected by the configured password. A client that
  // presents a wrong hash — or no hash at all when a password is set — is
  // refused here, before any command reaches the tuner.
  //
  // The FM-DX startup sequence is: the client sends "x", the server replies
  // "\nOK\n" (Dispatcher::cbStartup), and the client then sends its commands.
  // The XDR salt/hash exchange is inserted between those two steps: the
  // server sends its salt right after the startup reply and expects the
  // 40-character hash before accepting any further command.
  //
  // A client that never sends "x" (the reference sends it exactly once, in
  // its startup handshake) is refused.
  bool authenticated = false;
  bool awaitingAuth = false;
  if (!m_password.empty()) {
    // A password is configured: the salt/hash exchange is mandatory.
    awaitingAuth = true;
  } else {
    // No password: guest mode. The client is still allowed to command the
    // tuner, matching the XDR path's behaviour with an empty password.
    authenticated = true;
  }
  // The very first line must be the startup "x" and nothing else. A first
  // line that is some other payload is not a valid FM-DX client, and a
  // first line that is oversized is rejected outright.
  bool sawStartupLine = false;
  // True when this client is the one that put the tuner into play. A client
  // that connects to a tuner that is already running (--auto-start, or a
  // client that is already connected) does not own the tuner, so its
  // disconnect must not stop it: the programme would cut out in the middle
  // of the listener's music because a browser tab was closed.
  bool clientOwnsTuner = false;
  // The salt sent with the startup reply, and the deadline by which the
  // client must answer with the matching hash.
  std::string fmdxSalt;
  auto fmdxAuthDeadline =
      std::chrono::steady_clock::time_point::max();

  while (m_running && !disconnectRequested) {
    const auto n =
        recv(nativeSocket(clientSocket), cmdBuffer, sizeof(cmdBuffer) - 1, 0);
    if (n < 0) {
      if (socketWouldBlock(lastSocketError())) {
        // A client that stalls during the auth exchange is dropped once the
        // handshake deadline passes, so it cannot pin this thread.
        if (awaitingAuth &&
            std::chrono::steady_clock::now() >= fmdxAuthDeadline) {
          std::cout << "[XDR] FM-DX client disconnected before sending auth "
                       "hash\n";
          break;
        }
        continue;
      }
      break;
    }
    if (n == 0) {
      break;
    }

    cmdBuffer[n] = '\0';
    command += cmdBuffer;
    if (commandBufferOverflowed(command)) {
      if (m_verboseLogging.load()) {
        std::cout << "[XDR] closing FM-DX client due to oversized command line"
                  << "\n";
      }
      break;
    }

    size_t pos;
    while ((pos = command.find('\n')) != std::string::npos) {
      if (lineExceedsLimit(pos)) {
        if (m_verboseLogging.load()) {
          std::cout << "[XDR] closing FM-DX client due to oversized command line"
                    << "\n";
        }
        setRecvTimeoutMs(clientSocket, 0);
        return;
      }
      std::string line = command.substr(0, pos);
      command = command.substr(pos + 1);

      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }

      if (!sawStartupLine) {
        // The first line has to be the startup "x". Anything else means this
        // is not an FM-DX client after all (the protocol detection only
        // looked at the first *byte*).
        sawStartupLine = true;
        if (line != "x") {
          if (m_verboseLogging.load()) {
            std::cout << "[XDR] invalid FM-DX handshake payload" << "\n";
          }
          setRecvTimeoutMs(clientSocket, 0);
          return;
        }
      }

      if (line == "x") {
        if (tunerStarted) {
          // A second "x" is a duplicate startup line. Answer it the same
          // way so a client that re-sends the handshake is not left waiting.
          sendSocket(clientSocket, "\nOK\n", 4);
          continue;
        }
        tunerStarted = true;
        // The tuner is only "owned" by this client when it was not already
        // running: the stop-on-disconnect below must not cut a tuner that
        // somebody else (--auto-start, another client) is using.
        clientOwnsTuner =
            !(m_tunerActiveCallback && m_tunerActiveCallback());
        if (m_startCallback) {
          m_startCallback();
        }
        // The reference client's startup reply is "\nOK\n"
        // (Dispatcher::cbStartup). "1" was never part of the protocol.
        sendSocket(clientSocket, "\nOK\n", 4);

        if (awaitingAuth) {
          // Send the salt and require the hash before any further command.
          std::string salt = generateSalt();
          if (salt.empty()) {
            // Secure RNG unavailable: fail closed.
            std::cout << "[XDR] client disconnected (no salt available)\n";
            setRecvTimeoutMs(clientSocket, 0);
            return;
          }
          if (m_verboseLogging.load()) {
            std::cout << "[XDR] sending salt to FM-DX client: '" << salt << "'\n";
          }
          std::string saltMsg = salt + "\n";
          sendSocket(clientSocket, saltMsg.c_str(), saltMsg.length());
          fmdxSalt = salt;
          fmdxAuthDeadline = std::chrono::steady_clock::now() +
                             std::chrono::milliseconds(kHandshakeTimeoutMs);
        }
        continue;
      }

      if (awaitingAuth) {
        // The next line must be the auth hash. It is read from the same
        // line buffer as the commands, so a client that pipelines the
        // startup line and the hash in one write is handled correctly.
        std::string clientHash = line;
        if (clientHash.length() == HASH_LENGTH + 1 &&
            (clientHash[0] == 'P' || clientHash[0] == 'p')) {
          clientHash = clientHash.substr(1);
        }
        const bool authSuccess = authenticate(fmdxSalt, clientHash);
        if (m_verboseLogging.load()) {
          std::cout << "[XDR] FM-DX authentication "
                    << (authSuccess ? "SUCCESS" : "FAILED") << "\n";
        }
        if (!authSuccess) {
          // A wrong hash is refused. Guests are not offered on this path:
          // the reference FM-DX protocol has no read-only mode, so there is
          // nothing to downgrade to.
          sendSocket(clientSocket, "a0\n", 3);
          std::cout << "[XDR] client disconnected from " << clientIP << "\n";
          setRecvTimeoutMs(clientSocket, 0);
          return;
        }
        authenticated = true;
        awaitingAuth = false;
        continue;
      }

      if (!authenticated) {
        sendSocket(clientSocket, "a0\n", 3);
        disconnectRequested = true;
        break;
      }

      if (line == "X") {
        tunerStarted = false;
        if (m_stopCallback) {
          m_stopCallback();
        }
        sendSocket(clientSocket, "1\n", 2);
      } else if (tunerStarted) {
        std::string response = processFmdxCommand(line);
        response += "\n";
        if (sendSocket(clientSocket, response.c_str(), response.length()) <= 0) {
          disconnectRequested = true;
          break;
        }
      } else {
        if (sendSocket(clientSocket, "ER\n", 3) <= 0) {
          disconnectRequested = true;
          break;
        }
      }
    }
  }
  setRecvTimeoutMs(clientSocket, 0);

  if (tunerStarted && clientOwnsTuner && m_stopCallback) {
    m_stopCallback();
  }
}

void XDRServer::handleXdrClient(SocketHandle clientSocket,
                               const char *clientIP) {
  if (m_verboseLogging.load()) {
    std::cout << "[XDR] client authenticating from " << clientIP << "\n";
  }
  bool authenticated = false;
  bool guestSession = false;

  std::string salt = generateSalt();
  if (salt.empty()) {
    // Secure RNG unavailable; we already logged the cause. Without a salt we
    // cannot run the challenge/response, so close the connection.
    return;
  }

  if (m_verboseLogging.load()) {
    std::cout << "[XDR] sending salt: '" << salt << "'" << "\n";
  }

  std::string msg = salt + "\n";
  sendSocket(clientSocket, msg.c_str(), msg.length());

  std::string authMsg;
  // The socket's receive timeout is cleared at this point, which on POSIX
  // means "block forever". Bound the auth read so a client that connects and
  // then stalls cannot pin this thread indefinitely.
  setRecvTimeoutMs(clientSocket, kHandshakeTimeoutMs);
  const bool gotAuth =
      recvLine(clientSocket, authMsg, HASH_LENGTH + 2);
  setRecvTimeoutMs(clientSocket, 0);
  if (!gotAuth) {
    if (m_verboseLogging.load()) {
      std::cout << "[XDR] client disconnected before sending auth hash"
                << "\n";
    }
    return;
  }

  std::string clientHash = authMsg;
  if (clientHash.length() == HASH_LENGTH + 1 &&
      (clientHash[0] == 'P' || clientHash[0] == 'p')) {
    clientHash = clientHash.substr(1);
  }

  if (m_verboseLogging.load()) {
    std::cout << "[XDR] received auth payload: '" << authMsg << "' -> hash '"
              << clientHash << "' (length: " << clientHash.length() << ")"
              << "\n";
  }

  bool authSuccess = authenticate(salt, clientHash);
  if (m_verboseLogging.load()) {
    std::cout << "[XDR] authentication " << (authSuccess ? "SUCCESS" : "FAILED")
              << "\n";
  }

  if (!authSuccess && !m_guestMode) {
    sendSocket(clientSocket, "a0\n", 3);
    return;
  }

  if (!authSuccess && m_guestMode) {
    sendSocket(clientSocket, "a1\n", 3);
    guestSession = true;
  } else {
    sendSocket(clientSocket, "a2\n", 3);
    guestSession = false;
  }

  // A guest session is a *read-only* session, matching the reference daemon's
  // documented "guest login (read-only access)" (research/xdrd/xdrd.c:321).
  // Only a session that presented the correct hash is `authenticated`; a guest
  // is merely allowed to connect and observe.
  authenticated = authSuccess;
  const bool sessionEstablished = authSuccess || guestSession;
  uint64_t lastRdsSeq = 0;
  uint64_t lastScanSeq = 0;
  if (sessionEstablished) {
    {
      std::lock_guard<std::mutex> lock(m_rdsMutex);
      if (!m_rdsQueue.empty()) {
        lastRdsSeq = m_rdsQueue.back().first;
      }
    }
    {
      std::lock_guard<std::mutex> lock(m_scanMutex);
      if (!m_scanQueue.empty()) {
        lastScanSeq = m_scanQueue.back().first;
      }
    }
    // The counters are bumped before the line is built so the count this
    // client sees already includes itself.
    addOnlineClient(authSuccess);
    std::string online = onlineLine() + "\n";
    sendSocket(clientSocket, online.c_str(), online.length());
    // Tell the clients that were already connected that somebody joined.
    broadcastOnlineLine();

    std::string snapshot = buildXdrStateSnapshot();
    snapshot += "\n";
    sendSocket(clientSocket, snapshot.c_str(), snapshot.length());
  }

  char cmdBuffer[256];
  std::string command;
  setRecvTimeoutMs(clientSocket, 100);
  auto lastSignal = std::chrono::steady_clock::now();

  while (m_running) {
    const auto n =
        recv(nativeSocket(clientSocket), cmdBuffer, sizeof(cmdBuffer) - 1, 0);
    if (n < 0) {
      if (socketWouldBlock(lastSocketError())) {
        bool sendFailed = false;
        std::vector<std::string> rdsLines;
        {
          std::lock_guard<std::mutex> lock(m_rdsMutex);
          if (!m_rdsQueue.empty() &&
              m_rdsQueue.back().first > lastRdsSeq) {
            for (const auto &entry : m_rdsQueue) {
              if (entry.first > lastRdsSeq) {
                rdsLines.push_back(entry.second);
                lastRdsSeq = entry.first;
              }
            }
          }
        }
        for (const std::string &rdsLineBase : rdsLines) {
          std::string rdsLine = rdsLineBase + "\n";
          if (sendSocket(clientSocket, rdsLine.c_str(), rdsLine.length()) <= 0) {
            sendFailed = true;
            break;
          }
        }
        if (sendFailed) {
          break;
        }

        std::vector<std::string> scanLines;
        {
          std::lock_guard<std::mutex> lock(m_scanMutex);
          if (!m_scanQueue.empty() &&
              m_scanQueue.back().first > lastScanSeq) {
            for (const auto &entry : m_scanQueue) {
              if (entry.first > lastScanSeq) {
                scanLines.push_back(entry.second);
                lastScanSeq = entry.first;
              }
            }
          }
        }
        for (const std::string &scanLineBase : scanLines) {
          std::string scanLine = scanLineBase + "\n";
          if (sendSocket(clientSocket, scanLine.c_str(), scanLine.length()) <=
              0) {
            sendFailed = true;
            break;
          }
        }
        if (sendFailed) {
          break;
        }

        int intervalMs = m_samplingInterval.load();
        if (intervalMs <= 0) {
          intervalMs = 66;
        }
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           now - lastSignal)
                           .count();
        if (elapsed >= intervalMs) {
          const uint64_t lastRdsMs =
              m_lastRdsMs.load(std::memory_order_relaxed);
          const uint64_t nowMs = steadyNowMs();
          if (lastRdsMs != 0 && nowMs - lastRdsMs <= 1500) {
            uint16_t piValue = 0xFFFF;
            {
              std::lock_guard<std::mutex> lock(m_rdsMutex);
              piValue = m_piLastValue;
            }
            if (piValue != 0xFFFF) {
              char piLine[16];
              std::snprintf(piLine, sizeof(piLine), "P%04X\n", piValue);
              if (sendSocket(clientSocket, piLine, std::strlen(piLine)) <= 0) {
                break;
              }
            }
          }

          std::string signalMsg = buildSignalLine();
          signalMsg += "\n";
          if (sendSocket(clientSocket, signalMsg.c_str(), signalMsg.length()) <=
              0) {
            break;
          }
          lastSignal = now;
        }
        continue;
      }
      break;
    }
    if (n == 0) {
      break;
    }

    cmdBuffer[n] = '\0';
    command += cmdBuffer;
    if (commandBufferOverflowed(command)) {
      if (m_verboseLogging.load()) {
        std::cout << "[XDR] closing XDR client due to oversized command line"
                  << "\n";
      }
      break;
    }

    size_t pos;
    while ((pos = command.find('\n')) != std::string::npos) {
      if (lineExceedsLimit(pos)) {
        if (m_verboseLogging.load()) {
          std::cout << "[XDR] closing XDR client due to oversized command line"
                    << "\n";
        }
        setRecvTimeoutMs(clientSocket, 0);
        return;
      }
      std::string line = command.substr(0, pos);
      command = command.substr(pos + 1);

      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }

      std::string response =
          processCommand(line, authenticated, guestSession, clientSocket);
      if (!response.empty()) {
        response += "\n";
        sendSocket(clientSocket, response.c_str(), response.length());
      }
    }
  }
  setRecvTimeoutMs(clientSocket, 0);

  // A disconnect cancels the scan only when this client was the one that
  // armed it. Unconditionally cancelling meant that any second client
  // connecting and hanging up aborted a sweep that belonged to somebody else.
  const SocketHandle scanOwner =
      m_scanOwnerSocket.load(std::memory_order_relaxed);
  if (scanOwner == kInvalidSocket || scanOwner == clientSocket) {
    m_scanCancelPending = true;
    if (scanOwner == clientSocket) {
      m_scanOwnerSocket.store(kInvalidSocket, std::memory_order_relaxed);
    }
  }

  if (sessionEstablished) {
    removeOnlineClient(authSuccess);
    broadcastOnlineLine();
  }
}

std::string XDRServer::processCommand(const std::string &cmd,
                                      bool authenticated, bool guestSession,
                                      SocketHandle clientSocket) {
  // guestSession is no longer consulted: privilege is decided solely by
  // `authenticated` (H1 made guests read-only). The parameter is kept so the
  // session role stays visible at the call site and in the tests that cover
  // the guest path.
  (void)guestSession;
  if (cmd.empty()) {
    m_scanCancelPending = true;
    return "";
  }

  char command = cmd[0];
  std::string arg = cmd.length() > 1 ? cmd.substr(1) : "";

  if (!authenticated) {
    // Guests are read-only, matching the reference daemon's documented
    // "guest login (read-only access)" (research/xdrd/xdrd.c:585 — only an
    // authenticated session may command the tuner). Read-only commands are
    // answered; anything that would reconfigure or retune the tuner is
    // rejected with "a0" (the protocol's "not authorized" reply).
    if (command == 'N' || command == 'o' || command == 'P') {
      // Fall through to the read-only handlers below.
    } else if (command == 'I' && arg.empty()) {
      // 'I' with no argument is a query for the current sampling interval;
      // 'I<int>,<detector>' changes it and is therefore not guest-allowed.
    } else {
      // The spectral scan ('S') is rejected for a guest like any other
      // tuner command: it retunes the radio across the band, and the
      // reference daemon likewise lets only an authenticated session drive
      // the tuner (research/xdrd/xdrd.c:585). Note that this costs the
      // default configuration nothing: with no password the server's
      // challenge/response accepts SHA1(salt) — which is exactly what the
      // reference client sends for an empty password — so such a client is
      // fully authenticated, not a guest.
      return "a0";
    }
  }

  std::lock_guard<std::mutex> lock(m_callbackMutex);

  switch (command) {
  case 'x': {
    if (m_startCallback) {
      m_startCallback();
    }
    return "OK";
  }

  case 'X': {
    m_scanCancelPending = true;
    if (m_stopCallback) {
      m_stopCallback();
    }
    return "X";
  }

  case 'P':
    return "a2";

  case 'S':
    // TEF-style scan control:
    // Sa<start_kHz> Sb<stop_kHz> Sc<step_kHz> Sw<bw_hz> Sz<antenna> S / Sm
    if (arg.empty()) {
      m_scanContinuous = false;
      m_scanStartPending = true;
      m_scanOwnerSocket.store(clientSocket, std::memory_order_relaxed);
      return "";
    }
    if (arg == "m") {
      m_scanContinuous = true;
      m_scanStartPending = true;
      m_scanOwnerSocket.store(clientSocket, std::memory_order_relaxed);
      return "";
    }
    if (arg.size() >= 2) {
      const char sub = arg[0];
      int value = 0;
      if (parseIntValue(arg.substr(1), value)) {
        switch (sub) {
        case 'a':
          m_scanStartKHz =
              std::clamp(value, static_cast<int>(fm_tuner::kFmBroadcastMinFreqKHz),
                         static_cast<int>(fm_tuner::kFmBroadcastMaxFreqKHz));
          break;
        case 'b':
          m_scanStopKHz =
              std::clamp(value, static_cast<int>(fm_tuner::kFmBroadcastMinFreqKHz),
                         static_cast<int>(fm_tuner::kFmBroadcastMaxFreqKHz));
          break;
        case 'c':
          m_scanStepKHz = std::clamp(value, 5, 1000);
          break;
        case 'w':
          m_scanBandwidthHz = std::clamp(value, 0, 400000);
          break;
        case 'z':
          m_scanAntenna = std::clamp(value, 0, 9);
          break;
        case 'f':
          break;
        default:
          break;
        }
      }
    }
    return "";

  case 'T': {
    uint32_t freqHz = 0;
    if (!parseFrequencyHz(arg, freqHz)) {
      // A malformed tune is answered with the current frequency rather than
      // silence, so the client can tell "rejected" from "not yet applied".
      if (m_verboseLogging.load()) {
        std::cout << "[XDR] rejected malformed tune command: T" << arg << "\n";
      }
      return "T" + std::to_string(m_frequency.load() / 1000);
    }
    if (!fm_tuner::isValidFmBroadcastFreqHz(freqHz)) {
      if (m_verboseLogging.load()) {
        std::cout << "[XDR] rejected out-of-band tune command: T" << arg
                  << " (" << freqHz << " Hz)\n";
      }
      return "T" + std::to_string(m_frequency.load() / 1000);
    }
    // Reset the PI history before the new frequency is published, so the
    // previous station's PI code is not reported as the new station's.
    resetPiHistory();
    m_lastAppliedFrequency = freqHz;
    m_frequency = freqHz;
    if (m_freqCallback) {
      m_freqCallback(freqHz);
    }
    return "T" + std::to_string(freqHz / 1000);
  }

  case 'Y': {
    int volume = 0;
    if (!parseIntValue(arg, volume)) {
      return "";
    }
    volume = std::clamp(volume, 0, 100);
    m_volume = volume;
    if (m_volCallback) {
      m_volCallback(volume);
    }
    return "Y" + std::to_string(volume);
  }

  case 'V': {
    int daa = 0;
    if (!parseIntValue(arg, daa)) {
      return "";
    }
    // TEF alignment: 6 dB steps up to 36 dB.
    daa = std::clamp(daa, 0, 36);
    daa = ((daa + 3) / 6) * 6;
    daa = std::clamp(daa, 0, 36);
    m_daa = daa;
    if (m_alignmentCallback) {
      m_alignmentCallback(daa);
    }
    return "V" + std::to_string(daa);
  }

  case 'G': {
    int gain = 0;
    if (!parseTefCustomValue(arg, gain)) {
      return "";
    }
    bool accepted = true;
    if (m_gainCallback) {
      accepted = m_gainCallback(gain);
    }
    if (!accepted) {
      return formatGain(m_gain);
    }
    m_gain = gain;
    return formatGain(gain);
  }

  case 'A': {
    int agcMode = 0;
    if (!parseIntValue(arg, agcMode)) {
      return "";
    }
    agcMode = std::clamp(agcMode, 0, 3);
    bool accepted = true;
    if (m_agcCallback) {
      accepted = m_agcCallback(agcMode);
    }
    if (!accepted) {
      return "A" + std::to_string(m_agcMode.load());
    }
    m_agcMode = agcMode;
    return "A" + std::to_string(agcMode);
  }

  case 'M': {
    int mode = 0;
    if (!parseIntValue(arg, mode)) {
      return "";
    }
    mode = std::clamp(mode, 0, 1);
    m_mode = mode;
    if (m_modeCallback) {
      m_modeCallback(mode);
    }
    return "M" + std::to_string(mode);
  }

  case 'D': {
    int deemph = 0;
    if (!parseIntValue(arg, deemph)) {
      return "";
    }
    deemph = std::clamp(deemph, 0, 2);
    m_deemphasis = deemph;
    if (m_deemphasisCallback) {
      m_deemphasisCallback(deemph);
    }
    return "D" + std::to_string(deemph);
  }

  case 'F': {
    // 'F' is a prefix for tuner-extension subcommands (TEF-style backends
    // historically reported bandwidth here, hence the original no-op
    // placeholder). 'Fb<n>' selects stereo blend mode (0=soft, 1=normal,
    // 2=aggressive). Other 'F<x>' codes left as no-op for forward
    // compatibility.
    if (!arg.empty() && arg[0] == 'b') {
      int blend = 0;
      if (!parseIntValue(arg.substr(1), blend)) {
        return "";
      }
      blend = std::clamp(blend, 0, 2);
      if (m_blendModeCallback) {
        m_blendModeCallback(blend);
      }
      return std::string("Fb") + std::to_string(blend);
    }
    return "";
  }

  case 'W': {
    int bandwidth = 0;
    if (!parseIntValue(arg, bandwidth)) {
      return "";
    }
    // Clamp to the range the tuner can actually measure. The signal-meter
    // estimator analyses the channel with an 8192-point FFT; a bandwidth at
    // or above the sample rate makes the side-band window wrap back over
    // the channel, so the meter would measure the signal as its own noise
    // floor and report a strong station as no signal.
    bandwidth = std::clamp(bandwidth, 0, kMaxChannelBandwidthHz);
    m_bandwidth = bandwidth;
    if (m_bandwidthCallback) {
      m_bandwidthCallback(bandwidth);
    }
    return "W" + std::to_string(bandwidth);
  }

  case 'Z': {
    int antenna = 0;
    if (!parseIntValue(arg, antenna)) {
      return "";
    }
    antenna = std::clamp(antenna, 0, 9);
    m_antenna = antenna;
    if (m_antennaCallback) {
      m_antennaCallback(antenna);
    }
    return "Z" + std::to_string(antenna);
  }

  case 'Q': {
    int squelch = 0;
    if (!parseIntValue(arg, squelch)) {
      return "";
    }
    squelch = std::clamp(squelch, 0, 100);
    m_squelch = squelch;
    if (m_squelchCallback) {
      m_squelchCallback(squelch);
    }
    return "Q" + std::to_string(squelch);
  }

  case 'C': {
    int rotator = 0;
    if (!parseIntValue(arg, rotator)) {
      return "";
    }
    rotator = std::clamp(rotator, -1, 1);
    m_rotator = rotator;
    if (m_rotatorCallback) {
      m_rotatorCallback(rotator);
    }
    return "C" + std::to_string(rotator);
  }

  case 'I': {
    if (arg.empty()) {
      return formatSampling(m_samplingInterval, m_detector);
    }

    int interval = 0;
    int detector = m_detector;
    size_t comma = arg.find(',');
    if (comma == std::string::npos) {
      if (!parseIntValue(arg, interval)) {
        return "";
      }
    } else {
      if (!parseIntValue(arg.substr(0, comma), interval) ||
          !parseIntValue(arg.substr(comma + 1), detector)) {
        return "";
      }
    }

    interval = std::clamp(interval, 0, 1000);
    detector = std::clamp(detector, 0, 1);
    m_samplingInterval = interval;
    m_detector = detector;
    if (m_samplingCallback) {
      m_samplingCallback(interval, detector);
    }
    return formatSampling(interval, detector);
  }

  case 'B': {
    int forceMono = 0;
    if (!parseIntValue(arg, forceMono)) {
      return "";
    }
    bool mono = (forceMono != 0);
    m_forceMono = mono;
    if (m_forceMonoCallback) {
      m_forceMonoCallback(mono);
    }
    return std::string("B") + (mono ? "1" : "0");
  }

  case 'N':
    return "N" + std::to_string(m_pilotTenthsKHz.load());

  case 'o':
    // The real number of connected clients, not a per-session guess.
    return onlineLine();

  default:
    return "";
  }
}

void XDRServer::setFrequencyCallback(FrequencyCallback cb) {
  assignCallback(m_freqCallback, std::move(cb));
}
void XDRServer::setVolumeCallback(VolumeCallback cb) {
  assignCallback(m_volCallback, std::move(cb));
}
void XDRServer::setGainCallback(GainCallback cb) {
  assignCallback(m_gainCallback, std::move(cb));
}
void XDRServer::setAGCCallback(AGCCallback cb) {
  assignCallback(m_agcCallback, std::move(cb));
}
void XDRServer::setModeCallback(IntCallback cb) {
  assignCallback(m_modeCallback, std::move(cb));
}
void XDRServer::setDeemphasisCallback(IntCallback cb) {
  assignCallback(m_deemphasisCallback, std::move(cb));
}
void XDRServer::setFilterCallback(IntCallback cb) {
  assignCallback(m_filterCallback, std::move(cb));
}
void XDRServer::setBandwidthCallback(IntCallback cb) {
  assignCallback(m_bandwidthCallback, std::move(cb));
}
void XDRServer::setAntennaCallback(IntCallback cb) {
  assignCallback(m_antennaCallback, std::move(cb));
}
void XDRServer::setSquelchCallback(IntCallback cb) {
  assignCallback(m_squelchCallback, std::move(cb));
}
void XDRServer::setRotatorCallback(IntCallback cb) {
  assignCallback(m_rotatorCallback, std::move(cb));
}
void XDRServer::setAlignmentCallback(IntCallback cb) {
  assignCallback(m_alignmentCallback, std::move(cb));
}
void XDRServer::setSamplingCallback(SamplingCallback cb) {
  assignCallback(m_samplingCallback, std::move(cb));
}
void XDRServer::setBlendModeCallback(IntCallback cb) {
  assignCallback(m_blendModeCallback, std::move(cb));
}
void XDRServer::setForceMonoCallback(ForceMonoCallback cb) {
  assignCallback(m_forceMonoCallback, std::move(cb));
}
void XDRServer::setStartCallback(StartCallback cb) {
  assignCallback(m_startCallback, std::move(cb));
}
void XDRServer::setStopCallback(StopCallback cb) {
  assignCallback(m_stopCallback, std::move(cb));
}

void XDRServer::setTunerActiveCallback(TunerActiveCallback cb) {
  assignCallback(m_tunerActiveCallback, std::move(cb));
}

std::string XDRServer::processFmdxCommand(const std::string &cmd) {
  if (cmd.empty()) {
    return "";
  }

  char command = cmd[0];
  std::string arg = cmd.length() > 1 ? cmd.substr(1) : "";

  std::lock_guard<std::mutex> lock(m_callbackMutex);

  switch (command) {
  case 'T': {
    uint32_t freqHz = 0;
    if (!parseFrequencyHz(arg, freqHz)) {
      return "ER";
    }
    // Same FM-band guard as the XDR-GTK 'T' handler. Without it an FM-DX
    // client could drive the tuner off-band (parseFrequencyHz reads a value
    // below 1000000 as kHz, so "T10" is 10 kHz and "T999999999" is ~1 GHz).
    if (!fm_tuner::isValidFmBroadcastFreqHz(freqHz)) {
      return "ER";
    }
    resetPiHistory();
    m_lastAppliedFrequency = freqHz;
    m_frequency = freqHz;
    if (m_freqCallback) {
      m_freqCallback(freqHz);
    }
    return "OK";
  }

  case 'Y': {
    int vol = 0;
    if (!parseIntValue(arg, vol)) {
      return "ER";
    }
    vol = std::clamp(vol, -100, 100);
    int mapped = std::clamp((vol + 100) / 2, 0, 100);
    m_volume = mapped;
    if (m_volCallback) {
      m_volCallback(mapped);
    }
    return "OK";
  }

  case 'A': {
    int agc = 0;
    if (!parseIntValue(arg, agc)) {
      return "ER";
    }
    agc = std::clamp(agc, 0, 2);
    bool accepted = true;
    if (m_agcCallback) {
      accepted = m_agcCallback(agc);
    }
    if (!accepted) {
      return "ER";
    }
    m_agcMode = agc;
    return "OK";
  }

  case 'I': {
    std::ostringstream oss;
    oss << "F=" << m_frequency;
    return oss.str();
  }

  case 'B': {
    // Force mono. Mirrors processCommand's 'B' handler in the XDR-GTK path so
    // FM-DX clients (which speak this protocol after the 'x' handshake) can
    // also toggle mono. Returns "OK" / "ER" to match the FM-DX convention.
    int forceMono = 0;
    if (!parseIntValue(arg, forceMono)) {
      return "ER";
    }
    const bool mono = (forceMono != 0);
    m_forceMono = mono;
    if (m_forceMonoCallback) {
      m_forceMonoCallback(mono);
    }
    return "OK";
  }

  case 'W': {
    // Channel bandwidth in Hz — same units as processCommand's 'W'.
    int bandwidth = 0;
    if (!parseIntValue(arg, bandwidth)) {
      return "ER";
    }
    // Same clamp as the XDR-GTK 'W' handler: a bandwidth at or above the
    // sample rate cannot be measured by the signal meter.
    bandwidth = std::clamp(bandwidth, 0, kMaxChannelBandwidthHz);
    m_bandwidth = bandwidth;
    if (m_bandwidthCallback) {
      m_bandwidthCallback(bandwidth);
    }
    return "OK";
  }

  case 'F': {
    // Tuner-extension subcommand prefix. Currently supports stereo blend
    // selection via 'Fb<n>' (0=soft, 1=normal, 2=aggressive), matching the
    // XDR-GTK protocol path. Other 'F<x>' codes are reserved.
    if (!arg.empty() && arg[0] == 'b') {
      int blend = 0;
      if (!parseIntValue(arg.substr(1), blend)) {
        return "ER";
      }
      blend = std::clamp(blend, 0, 2);
      if (m_blendModeCallback) {
        m_blendModeCallback(blend);
      }
      return "OK";
    }
    return "ER";
  }

  case 'D': {
    // De-emphasis mode (0 = 50us, 1 = 75us, 2 = off).
    int deemph = 0;
    if (!parseIntValue(arg, deemph)) {
      return "ER";
    }
    deemph = std::clamp(deemph, 0, 2);
    m_deemphasis = deemph;
    if (m_deemphasisCallback) {
      m_deemphasisCallback(deemph);
    }
    return "OK";
  }

  default:
    return "ER";
  }
}
