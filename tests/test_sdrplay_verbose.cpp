#include "catch_compat.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "sdrplay_device.h"
#include "tuner_controller.h"

#if !defined(_WIN32)
#include <unistd.h>
#endif

// These tests compile WITH FM_TUNER_HAS_SDRPLAY, so the device has a real
// connect() path. They cover the verbose-gating of the device's own
// diagnostics: the connect trace is useful when something goes wrong, but it
// must not be printed unconditionally — the rest of the app routes its log
// lines through the verbose flag, and a connect attempt on a machine without
// the SDRplay API used to print to stderr even under -q.

#if !defined(_WIN32)

namespace {

// Redirects stderr into a temp file for the lifetime of the object.
class StderrCapture {
public:
  bool start() {
    char tmpl[] = "/tmp/fm_sdrplay_stderr_XXXXXX";
    m_tmpFd = mkstemp(tmpl);
    if (m_tmpFd < 0) {
      return false;
    }
    m_path = tmpl;
    std::fflush(stderr);
    m_savedFd = dup(STDERR_FILENO);
    if (m_savedFd < 0) {
      close(m_tmpFd);
      m_tmpFd = -1;
      return false;
    }
    dup2(m_tmpFd, STDERR_FILENO);
    return true;
  }

  std::string stop() {
    if (m_tmpFd < 0) {
      return std::string();
    }
    std::fflush(stderr);
    dup2(m_savedFd, STDERR_FILENO);
    close(m_savedFd);
    m_savedFd = -1;
    close(m_tmpFd);
    m_tmpFd = -1;

    std::string out;
    FILE *f = std::fopen(m_path.c_str(), "rb");
    if (f) {
      char buf[512];
      size_t n;
      while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, n);
      }
      std::fclose(f);
    }
    unlink(m_path.c_str());
    return out;
  }

private:
  int m_savedFd = -1;
  int m_tmpFd = -1;
  std::string m_path;
};

} // namespace

TEST_CASE("SDRplay connect diagnostics honour the verbose flag",
          "[sdrplay][verbose]") {
  // Quiet: the connect trace is suppressed entirely. The device may or may
  // not reach the hardware (no RSP attached in CI), but nothing it prints on
  // the way there is user-visible.
  {
    SDRplayDevice dev;
    dev.setVerboseLogging(false);
    REQUIRE_FALSE(dev.verboseLogging());

    StderrCapture capture;
    REQUIRE(capture.start());
    dev.connect();
    const std::string captured = capture.stop();

    REQUIRE(captured.find("[SDRplay]") == std::string::npos);
  }

  // Verbose: the same connect attempt reports what it found. Every exit path
  // of connect() prints at least one [SDRplay] line, so this holds whether
  // the API library is missing, the service is down, no device is attached,
  // or a device is found.
  {
    SDRplayDevice dev;
    dev.setVerboseLogging(true);
    REQUIRE(dev.verboseLogging());

    StderrCapture capture;
    REQUIRE(capture.start());
    dev.connect();
    const std::string captured = capture.stop();

    REQUIRE(captured.find("[SDRplay]") != std::string::npos);
  }
}

TEST_CASE("TunerController forwards the verbose flag to the SDRplay source",
          "[sdrplay][verbose]") {
  TunerController tuner("sdrplay", "", 0, 0);
  REQUIRE(tuner.isSdrPlay());

  StderrCapture capture;
  REQUIRE(capture.start());
  tuner.setVerboseLogging(false);
  tuner.connect();
  const std::string quiet = capture.stop();
  REQUIRE(quiet.find("[SDRplay]") == std::string::npos);

  REQUIRE(capture.start());
  tuner.setVerboseLogging(true);
  tuner.connect();
  const std::string verbose = capture.stop();
  REQUIRE(verbose.find("[SDRplay]") != std::string::npos);
}

#endif // !_WIN32
